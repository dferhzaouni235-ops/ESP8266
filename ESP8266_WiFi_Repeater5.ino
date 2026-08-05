/*
====================================================================
  ESP8266 WiFi Range Extender / Repeater  —  موزّع / معيد بث واي فاي
====================================================================
  الفكرة:
  1) عند أول تشغيل (أو بعد إعادة الضبط) يفتح ESP8266 شبكة إعداد
     خاصة به وتظهر صفحة إعداد أنيقة تلقائيًا (Captive Portal) بمجرد
     الاتصال بها من الجوال أو الحاسوب.
  2) تختار من القائمة شبكة الراوتر (SSID) وتكتب كلمة سرها، ثم تكتب
     اسم وكلمة سر الشبكة الجديدة التي سيبثها ESP8266.
  3) بعد الحفظ يعيد الجهاز تشغيل نفسه، يتصل بالراوتر، ثم يبث شبكة
     واي فاي جديدة توزّع الإنترنت فعليًا (NAT حقيقي عبر NAPT) لأي
     جهاز بعيد عن الراوتر الأصلي.

  المميزات المضمّنة:
   • بوابة إعداد (Captive Portal) تُفتح تلقائيًا مع تصميم عصري متجاوب
   • فحص الشبكات المحيطة وعرضها مرتّبة حسب قوة الإشارة + أيقونة قفل
   • خيار إدخال اسم شبكة يدويًا (لشبكة مخفية)
   • توزيع إنترنت حقيقي عبر NAPT (وليس مجرد بث شبكة بلا إنترنت)
   • لوحة حالة (Dashboard) تتحدث تلقائيًا: قوة الإشارة، عدد الأجهزة
     المتصلة، زمن التشغيل، الذاكرة الحرة، حالة NAT
   • حماية لوحة الحالة وأزرار التحكم بكلمة مرور (Basic Auth)
   • إعادة اتصال تلقائي عند انقطاع الراوتر + إعادة تشغيل احترازية
     إذا فشلت المحاولات المتكررة (خوارزمية مراقبة/Watchdog)
   • دعم mDNS للوصول عبر http://esprepeater.local
   • مؤشر LED بأنماط وميض مختلفة لكل حالة
   • زر إعادة ضبط مصنعي (اضغط مطوّلًا 3 ثوانٍ على زر GPIO0/Flash)

  المكتبات المطلوبة (Arduino IDE -> Sketch -> Include Library -> Manage Libraries):
   - ArduinoJson (by Benoit Blanchon) — الإصدار 6 أو 7
   - باقي المكتبات (ESP8266WiFi, ESP8266WebServer, DNSServer, LittleFS,
     ESP8266mDNS) تأتي مدمجة مع "حزمة لوحات ESP8266" في Arduino IDE.

  ⚠️ إعدادات ضرورية في Arduino IDE قبل الرفع (Tools):
   - Board            : NodeMCU 1.0 (أو حسب لوحتك)
   - LwIP Variant      : "v2 Higher Bandwidth"
        (مهم جدًا — لا تختر "(no features)" ولا نسخة IPv6، لأن ميزة
         NAPT التي توزّع الإنترنت فعليًا تحتاج بالضبط هذا الخيار)
   - Flash Size        : اختر خيارًا يحجز مساحة لنظام ملفات LittleFS
        (مثال: "4MB (FS:1MB OTA:~1019KB)")
   - Erase Flash        : "Only Sketch" في الرفعات العادية

  مؤشرات LED:
   - وميض بطيء (كل ثانية)  = وضع الإعداد (بانتظار إدخال بياناتك)
   - إضاءة ثابتة            = يحاول الاتصال بالراوتر الآن
   - وميض سريع (كل 200ms)   = متصل ويوزّع الإنترنت بنجاح
   - انطفاء تام              = تعذّر الاتصال / NAPT غير مدعوم

====================================================================
*/

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <ESP8266mDNS.h>
#include <vector>

// ---- دعم NAPT (توزيع الإنترنت الحقيقي) ----
#if LWIP_FEATURES && !LWIP_IPV6
  #define NAPT_SUPPORTED 1
  #include <lwip/napt.h>
  #include <lwip/dns.h>
  #include <LwipDhcpServer.h>
  #define NAPT_ENTRIES 1000
  #define NAPT_PORTS   10
#else
  #define NAPT_SUPPORTED 0
#endif

// ==================== إعدادات عامة ====================
#define LED_PIN            LED_BUILTIN   // LED مدمج (Active LOW على أغلب لوحات ESP8266)
#define RESET_BUTTON_PIN   0             // زر GPIO0 / FLASH
#define DEFAULT_AP_NAME    "ESP-Repeater-Setup"   // اسم شبكة الإعداد الأولى
#define CONFIG_FILE        "/config.json"
#define ADMIN_USER         "admin"

const IPAddress SETUP_AP_IP(192, 168, 4, 1);
const IPAddress REPEATER_AP_IP(192, 168, 50, 1);
const byte DNS_PORT = 53;

// اسم شبكة الإعداد وكلمة سرها (تُبنى في startSetupMode بإضافة كود فريد لهذا الجهاز)
String setupApSsid;
String setupApPass;

ESP8266WebServer server(80);
DNSServer dnsServer;

// ==================== حالة النظام ====================
String routerSSID, routerPASS, apName, apPass;
bool   configExists     = false;
bool   repeaterModeActive = false;
bool   natEnabled        = false;

bool   restartPending = false;
unsigned long restartAt = 0;

uint8_t reconnectFails = 0;
unsigned long lastStaCheck = 0;
const uint8_t MAX_RECONNECT_FAILS = 15; // نحو 2.5 دقيقة من المحاولات قبل إعادة تشغيل كاملة

enum LedMode { LED_SETUP, LED_CONNECTING, LED_CONNECTED, LED_ERROR };
LedMode ledMode = LED_SETUP;
unsigned long lastBlink = 0;
bool ledState = false;

bool resetButtonHeld = false;
unsigned long resetHoldStart = 0;

// ==================== دوال مساعدة ====================
int rssiToPercent(int32_t rssi) {
  if (rssi <= -100) return 0;
  if (rssi >= -50)  return 100;
  return 2 * (rssi + 100);
}

String rssiToQuality(int32_t rssi) {
  if (rssi >= -50) return "ممتاز";
  if (rssi >= -60) return "جيد جدًا";
  if (rssi >= -70) return "جيد";
  if (rssi >= -80) return "ضعيف";
  return "ضعيف جدًا";
}

bool checkAuth() {
  if (!server.authenticate(ADMIN_USER, apPass.c_str())) {
    server.requestAuthentication();
    return false;
  }
  return true;
}

// ==================== تحميل/حفظ الإعدادات ====================
void loadConfig() {
  configExists = false;
  if (!LittleFS.exists(CONFIG_FILE)) return;

  File f = LittleFS.open(CONFIG_FILE, "r");
  if (!f) return;

  DynamicJsonDocument doc(512);
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    Serial.println("⚠️ ملف الإعدادات تالف، سيتم تجاهله");
    return;
  }

  routerSSID = doc["ssid"]   | "";
  routerPASS = doc["pass"]   | "";
  apName     = doc["apName"] | "";
  apPass     = doc["apPass"] | "";

  if (routerSSID.length() == 0 || apName.length() == 0) return;
  configExists = true;
}

bool saveConfig() {
  DynamicJsonDocument doc(512);
  doc["ssid"]   = routerSSID;
  doc["pass"]   = routerPASS;
  doc["apName"] = apName;
  doc["apPass"] = apPass;

  File f = LittleFS.open(CONFIG_FILE, "w");
  if (!f) return false;
  bool ok = serializeJson(doc, f) > 0;
  f.close();
  return ok;
}

// ==================== صفحة الإعداد (HTML) ====================
const char PAGE_SETUP[] PROGMEM = R"HTMLDELIM(
<!DOCTYPE html>
<html lang="ar" dir="rtl">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0">
<title>إعداد موزّع الإنترنت</title>
<style>
  * { box-sizing: border-box; margin:0; padding:0; }
  body {
    font-family: 'Segoe UI', Tahoma, Arial, sans-serif;
    min-height: 100vh;
    background: linear-gradient(135deg, #1e1b4b 0%, #4338ca 45%, #7c3aed 100%);
    display:flex; align-items:center; justify-content:center;
    padding: 24px 14px;
    color:#f3f4f6;
  }
  .card {
    width:100%; max-width:460px;
    background: rgba(255,255,255,0.08);
    backdrop-filter: blur(14px);
    border:1px solid rgba(255,255,255,0.18);
    border-radius:22px;
    padding:28px 24px 24px;
    box-shadow: 0 20px 50px rgba(0,0,0,0.35);
  }
  .logo { text-align:center; font-size:44px; margin-bottom:6px; }
  h1 { text-align:center; font-size:20px; font-weight:700; margin-bottom:4px; }
  .sub { text-align:center; color:#c7c9f5; font-size:13px; margin-bottom:22px; }
  label { display:block; font-size:13px; color:#d9d9ff; margin:14px 0 6px; }
  input[type=text], input[type=password] {
    width:100%; padding:12px 14px; border-radius:12px; border:1px solid rgba(255,255,255,0.25);
    background:rgba(255,255,255,0.10); color:#fff; font-size:14px; outline:none;
    transition:.2s;
  }
  input::placeholder { color:#b9baf0; }
  input:focus { border-color:#a78bfa; background:rgba(255,255,255,0.16); }
  .net-list { max-height:190px; overflow-y:auto; border-radius:12px; border:1px solid rgba(255,255,255,0.2); margin-top:6px; }
  .net-item {
    display:flex; align-items:center; justify-content:space-between;
    padding:11px 12px; cursor:pointer; border-bottom:1px solid rgba(255,255,255,0.08);
    font-size:13.5px; transition:.15s;
  }
  .net-item:last-child { border-bottom:none; }
  .net-item:hover { background:rgba(255,255,255,0.10); }
  .net-item.selected { background:rgba(124,58,237,0.45); }
  .net-name { display:flex; align-items:center; gap:8px; overflow:hidden; text-overflow:ellipsis; white-space:nowrap; }
  .bars { display:flex; gap:2px; align-items:flex-end; height:14px; }
  .bars span { width:3px; background:#6b7280; border-radius:1px; }
  .bars span.on { background:#4ade80; }
  .row-between { display:flex; align-items:center; justify-content:space-between; }
  .link-btn { background:none; border:none; color:#c4b5fd; font-size:12.5px; cursor:pointer; text-decoration:underline; }
  .toggle-manual { font-size:12.5px; color:#c4b5fd; margin-top:8px; display:inline-block; cursor:pointer; }
  .hint { font-size:11.5px; color:#a5a6d6; margin-top:5px; }
  button.submit {
    width:100%; margin-top:22px; padding:14px; border:none; border-radius:14px;
    background: linear-gradient(90deg, #7c3aed, #6366f1); color:#fff; font-size:15px; font-weight:700;
    cursor:pointer; box-shadow:0 8px 20px rgba(124,58,237,0.4); transition:.15s;
  }
  button.submit:active { transform: scale(0.98); }
  .msg { margin-top:14px; font-size:13px; text-align:center; min-height:18px; }
  .msg.err { color:#fca5a5; }
  .msg.ok  { color:#86efac; }
  .spinner {
    width:16px; height:16px; border:2.5px solid rgba(255,255,255,0.3); border-top-color:#fff;
    border-radius:50%; display:inline-block; animation:spin .7s linear infinite; vertical-align:middle; margin-left:6px;
  }
  @keyframes spin { to { transform: rotate(360deg); } }
  .success-screen { display:none; text-align:center; }
  .success-screen .emoji { font-size:52px; margin-bottom:10px; }
  footer { text-align:center; font-size:11px; color:#9a9bd6; margin-top:18px; }
</style>
</head>
<body>
  <div class="card" id="mainCard">
    <div class="logo">📡</div>
    <h1>إعداد موزّع الإنترنت</h1>
    <div class="sub">اربط الجهاز بالراوتر وابثّ شبكة جديدة بعيدة المدى</div>

    <div id="formArea">
      <label class="row-between">
        <span>شبكة الراوتر (Wi-Fi)</span>
        <button type="button" class="link-btn" onclick="scanNetworks()">🔄 إعادة فحص</button>
      </label>
      <div class="net-list" id="netList">
        <div class="net-item"><span>جارٍ البحث عن الشبكات… <span class="spinner"></span></span></div>
      </div>
      <span class="toggle-manual" onclick="toggleManual()">✏️ الشبكة غير ظاهرة؟ أدخل الاسم يدويًا</span>
      <div id="manualBox" style="display:none;">
        <label>اسم شبكة الراوتر (SSID)</label>
        <input type="text" id="manualSsid" placeholder="اسم الشبكة">
      </div>

      <label>كلمة سر شبكة الراوتر</label>
      <input type="password" id="routerPass" placeholder="اتركها فارغة إن كانت الشبكة مفتوحة">

      <label>اسم الشبكة الجديدة التي سيبثها الجهاز</label>
      <input type="text" id="apNameInput" placeholder="مثال: بيتنا-الطابق-الثاني" maxlength="31">

      <label>كلمة سر الشبكة الجديدة</label>
      <input type="password" id="apPassInput" placeholder="8 أحرف على الأقل (أو اتركها فارغة لشبكة مفتوحة)">
      <div class="hint">تُستخدم أيضًا ككلمة مرور للوصول للوحة التحكم لاحقًا</div>

      <button class="submit" onclick="submitConfig()">🚀 حفظ والاتصال</button>
      <div class="msg" id="msg"></div>
    </div>

    <div class="success-screen" id="successScreen">
      <div class="emoji">✅</div>
      <div style="font-weight:700; font-size:16px;">تم الحفظ بنجاح!</div>
      <div class="sub" style="margin-top:8px;">الجهاز يعيد التشغيل الآن ويتصل بالراوتر…<br>
      بعد قليل ابحث عن شبكة باسم:<br><b id="apNameShow" style="color:#c4b5fd"></b></div>
    </div>

    <footer>ESP8266 WiFi Repeater</footer>
  </div>

<script>
let selectedSsid = "";
let manualMode = false;
let scanTimer = null;

function toggleManual(){
  manualMode = !manualMode;
  document.getElementById('manualBox').style.display = manualMode ? 'block' : 'none';
  document.getElementById('netList').style.display = manualMode ? 'none' : 'block';
}

function barsHtml(percent){
  let bars = [4,7,10,13];
  let on = Math.round(percent/25);
  let html = '<div class="bars">';
  bars.forEach((h,i)=> html += `<span style="height:${h}px" class="${i < on ? 'on':''}"></span>`);
  html += '</div>';
  return html;
}

function renderNetworks(list){
  const el = document.getElementById('netList');
  if(!list.length){ el.innerHTML = '<div class="net-item"><span>لا توجد شبكات، جرّب إعادة الفحص</span></div>'; return; }
  el.innerHTML = '';
  list.forEach(n=>{
    const div = document.createElement('div');
    div.className = 'net-item' + (n.ssid===selectedSsid ? ' selected':'');
    div.innerHTML = `<span class="net-name">${n.secure?'🔒':'🔓'} ${n.ssid}</span>${barsHtml(n.percent)}`;
    div.onclick = ()=>{
      selectedSsid = n.ssid;
      document.querySelectorAll('.net-item').forEach(e=>e.classList.remove('selected'));
      div.classList.add('selected');
    };
    el.appendChild(div);
  });
}

function scanNetworks(){
  document.getElementById('netList').innerHTML = '<div class="net-item"><span>جارٍ البحث عن الشبكات… <span class="spinner"></span></span></div>';
  clearTimeout(scanTimer);
  poll();
}

function poll(){
  fetch('/scan').then(r=>r.json()).then(d=>{
    if(d.status === 'scanning'){
      scanTimer = setTimeout(poll, 1200);
    } else {
      renderNetworks(d.networks || []);
    }
  }).catch(()=>{ scanTimer = setTimeout(poll, 2000); });
}

function setMsg(text, cls){
  const m = document.getElementById('msg');
  m.textContent = text;
  m.className = 'msg ' + (cls||'');
}

function submitConfig(){
  const ssid = manualMode ? document.getElementById('manualSsid').value.trim() : selectedSsid;
  const apNameVal = document.getElementById('apNameInput').value.trim();
  const apPassVal = document.getElementById('apPassInput').value;
  const routerPassVal = document.getElementById('routerPass').value;

  if(!ssid){ setMsg('يرجى اختيار شبكة الراوتر أو كتابة اسمها يدويًا', 'err'); return; }
  if(!apNameVal){ setMsg('يرجى كتابة اسم للشبكة الجديدة', 'err'); return; }
  if(apPassVal.length > 0 && apPassVal.length < 8){ setMsg('كلمة سر الشبكة الجديدة يجب ألا تقل عن 8 خانات', 'err'); return; }

  setMsg('جارٍ الحفظ...', '');
  fetch('/save', {
    method:'POST',
    headers:{'Content-Type':'application/json'},
    body: JSON.stringify({ ssid, pass: routerPassVal, apName: apNameVal, apPass: apPassVal })
  }).then(r=>r.json()).then(d=>{
    if(d.ok){
      document.getElementById('formArea').style.display = 'none';
      document.getElementById('successScreen').style.display = 'block';
      document.getElementById('apNameShow').textContent = apNameVal;
    } else {
      setMsg(d.msg || 'حدث خطأ غير متوقع', 'err');
    }
  }).catch(()=> setMsg('تعذّر الاتصال بالجهاز، حاول مجددًا', 'err'));
}

scanNetworks();
</script>
</body>
</html>
)HTMLDELIM";

void handleSetupRoot() {
  server.send_P(200, "text/html", PAGE_SETUP);
}

// ==================== فحص الشبكات ====================
void handleScan() {
  int n = WiFi.scanComplete();

  if (n == -2) {
    WiFi.scanNetworks(true, false);
    server.send(200, "application/json", "{\"status\":\"scanning\"}");
    return;
  }
  if (n == -1) {
    server.send(200, "application/json", "{\"status\":\"scanning\"}");
    return;
  }

  // ترتيب الشبكات تنازليًا حسب قوة الإشارة (فرز فقاعي بسيط)
  std::vector<int> idx(n);
  for (int i = 0; i < n; i++) idx[i] = i;
  for (int i = 0; i < n - 1; i++) {
    for (int j = 0; j < n - 1 - i; j++) {
      if (WiFi.RSSI(idx[j]) < WiFi.RSSI(idx[j + 1])) {
        int t = idx[j]; idx[j] = idx[j + 1]; idx[j + 1] = t;
      }
    }
  }

  DynamicJsonDocument doc(4096);
  doc["status"] = "done";
  JsonArray arr = doc.createNestedArray("networks");
  String lastSsid = "";
  for (int i = 0; i < n; i++) {
    int k = idx[i];
    String ssid = WiFi.SSID(k);
    if (ssid.length() == 0 || ssid == lastSsid) continue; // تجاهل التكرار والشبكات المخفية بدون اسم
    lastSsid = ssid;
    JsonObject net = arr.createNestedObject();
    net["ssid"]    = ssid;
    net["rssi"]    = WiFi.RSSI(k);
    net["percent"] = rssiToPercent(WiFi.RSSI(k));
    net["secure"]  = (WiFi.encryptionType(k) != ENC_TYPE_NONE);
  }
  WiFi.scanDelete();

  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// ==================== حفظ الإعداد وإعادة التشغيل ====================
void handleSave() {
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"ok\":false,\"msg\":\"لا توجد بيانات مُرسلة\"}");
    return;
  }

  DynamicJsonDocument doc(512);
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"ok\":false,\"msg\":\"بيانات غير صالحة\"}");
    return;
  }

  String ssid  = String((const char*)(doc["ssid"] | "")); ssid.trim();
  String pass  = String((const char*)(doc["pass"] | ""));
  String ap    = String((const char*)(doc["apName"] | "")); ap.trim();
  String apPwd = String((const char*)(doc["apPass"] | ""));

  if (ssid.length() == 0) {
    server.send(400, "application/json", "{\"ok\":false,\"msg\":\"يجب اختيار أو كتابة اسم شبكة الراوتر\"}");
    return;
  }
  if (ap.length() == 0) {
    server.send(400, "application/json", "{\"ok\":false,\"msg\":\"يجب كتابة اسم للشبكة الجديدة\"}");
    return;
  }
  if (apPwd.length() > 0 && apPwd.length() < 8) {
    server.send(400, "application/json", "{\"ok\":false,\"msg\":\"كلمة سر الشبكة الجديدة يجب ألا تقل عن 8 خانات\"}");
    return;
  }

  routerSSID = ssid;
  routerPASS = pass;
  apName     = ap;
  apPass     = apPwd;

  if (!saveConfig()) {
    server.send(500, "application/json", "{\"ok\":false,\"msg\":\"فشل حفظ الإعدادات في الذاكرة\"}");
    return;
  }

  server.send(200, "application/json", "{\"ok\":true,\"msg\":\"تم الحفظ، جارٍ إعادة التشغيل\"}");
  dnsServer.stop();
  restartPending = true;
  restartAt = millis() + 1500;
}

// ==================== لوحة الحالة (Dashboard) ====================
const char PAGE_STATUS[] PROGMEM = R"HTMLDELIM(
<!DOCTYPE html>
<html lang="ar" dir="rtl">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>لوحة تحكم الموزّع</title>
<style>
  * { box-sizing:border-box; margin:0; padding:0; }
  body {
    font-family:'Segoe UI', Tahoma, Arial, sans-serif;
    background: linear-gradient(135deg, #0f172a 0%, #1e1b4b 50%, #312e81 100%);
    min-height:100vh; color:#f1f5f9; padding:24px 14px;
  }
  .wrap { max-width:520px; margin:0 auto; }
  h1 { text-align:center; font-size:21px; margin-bottom:18px; }
  h1 span { display:block; font-size:13px; color:#a5b4fc; font-weight:400; margin-top:4px; }
  .grid { display:grid; grid-template-columns:1fr 1fr; gap:12px; margin-bottom:14px; }
  .card {
    background:rgba(255,255,255,0.07); border:1px solid rgba(255,255,255,0.14);
    border-radius:16px; padding:16px; backdrop-filter:blur(10px);
  }
  .card .label { font-size:12px; color:#a5b4fc; margin-bottom:6px; }
  .card .value { font-size:19px; font-weight:700; }
  .badge { display:inline-block; padding:3px 10px; border-radius:20px; font-size:12px; font-weight:700; }
  .badge.on  { background:rgba(74,222,128,0.2); color:#4ade80; }
  .badge.off { background:rgba(248,113,113,0.2); color:#f87171; }
  .bar-bg { background:rgba(255,255,255,0.12); border-radius:8px; height:8px; margin-top:8px; overflow:hidden; }
  .bar-fill { height:100%; background:linear-gradient(90deg,#7c3aed,#4ade80); border-radius:8px; transition:.4s; }
  .actions { display:flex; gap:10px; margin-top:6px; }
  button {
    flex:1; padding:13px; border:none; border-radius:12px; font-size:14px; font-weight:700; cursor:pointer;
  }
  .btn-restart { background:rgba(99,102,241,0.25); color:#a5b4fc; border:1px solid rgba(99,102,241,0.4); }
  .btn-reset { background:rgba(248,113,113,0.18); color:#fca5a5; border:1px solid rgba(248,113,113,0.35); }
  footer { text-align:center; font-size:11px; color:#8789c9; margin-top:20px; }
</style>
</head>
<body>
<div class="wrap">
  <h1>📡 لوحة تحكم الموزّع <span id="apNameTitle">—</span></h1>

  <div class="grid">
    <div class="card"><div class="label">الاتصال بالراوتر</div><div class="value" id="staState">—</div></div>
    <div class="card"><div class="label">توزيع الإنترنت (NAT)</div><div class="value"><span class="badge" id="natBadge">—</span></div></div>
    <div class="card"><div class="label">قوة الإشارة</div><div class="value" id="rssiVal">—</div><div class="bar-bg"><div class="bar-fill" id="rssiBar" style="width:0%"></div></div></div>
    <div class="card"><div class="label">الأجهزة المتصلة بشبكتنا</div><div class="value" id="clients">—</div></div>
    <div class="card"><div class="label">وقت التشغيل</div><div class="value" id="uptime">—</div></div>
    <div class="card"><div class="label">الذاكرة الحرة</div><div class="value" id="heap">—</div></div>
    <div class="card"><div class="label">IP جهة الراوتر</div><div class="value" id="localIp" style="font-size:14px">—</div></div>
    <div class="card"><div class="label">IP الشبكة الجديدة</div><div class="value" id="apIp" style="font-size:14px">—</div></div>
  </div>

  <div class="actions">
    <button class="btn-restart" onclick="doAction('/restart','هل تريد إعادة تشغيل الجهاز؟')">🔁 إعادة تشغيل</button>
    <button class="btn-reset" onclick="doAction('/factory-reset','سيتم مسح كل الإعدادات المحفوظة، متابعة؟')">🗑️ ضبط المصنع</button>
  </div>

  <footer>يتحدّث تلقائيًا كل 3 ثوانٍ · ESP8266 WiFi Repeater</footer>
</div>

<script>
function fmtUptime(sec){
  const h = Math.floor(sec/3600), m = Math.floor((sec%3600)/60), s = sec%60;
  return `${h}س ${m}د ${s}ث`;
}
function refresh(){
  fetch('/api/status').then(r=>r.json()).then(d=>{
    document.getElementById('apNameTitle').textContent = '— ' + d.apName;
    document.getElementById('staState').textContent = d.staConnected ? ('متصل بـ ' + d.routerSsid) : 'غير متصل';
    const nat = document.getElementById('natBadge');
    nat.textContent = d.natEnabled ? 'مفعّل' : 'غير مفعّل';
    nat.className = 'badge ' + (d.natEnabled ? 'on':'off');
    document.getElementById('rssiVal').textContent = d.signalLabel + ' (' + d.rssi + ' dBm)';
    document.getElementById('rssiBar').style.width = d.signalPercent + '%';
    document.getElementById('clients').textContent = d.clients;
    document.getElementById('uptime').textContent = fmtUptime(d.uptimeSec);
    document.getElementById('heap').textContent = Math.round(d.freeHeap/1024) + ' KB';
    document.getElementById('localIp').textContent = d.localIp;
    document.getElementById('apIp').textContent = d.apIp;
  }).catch(()=>{});
}
function doAction(url, confirmMsg){
  if(!confirm(confirmMsg)) return;
  fetch(url, {method:'POST'}).then(()=>{
    alert('تم تنفيذ الأمر، سيعيد الجهاز الاتصال خلال لحظات...');
  });
}
refresh();
setInterval(refresh, 3000);
</script>
</body>
</html>
)HTMLDELIM";

void handleStatus() {
  if (!checkAuth()) return;
  server.send_P(200, "text/html", PAGE_STATUS);
}

void handleApiStatus() {
  if (!checkAuth()) return;
  DynamicJsonDocument doc(1024);
  bool sta = (WiFi.status() == WL_CONNECTED);
  doc["routerSsid"]    = routerSSID;
  doc["staConnected"]  = sta;
  doc["localIp"]       = sta ? WiFi.localIP().toString() : "-";
  doc["apIp"]          = WiFi.softAPIP().toString();
  doc["apName"]        = apName;
  doc["rssi"]          = sta ? WiFi.RSSI() : 0;
  doc["signalPercent"] = sta ? rssiToPercent(WiFi.RSSI()) : 0;
  doc["signalLabel"]   = sta ? rssiToQuality(WiFi.RSSI()) : "—";
  doc["clients"]       = WiFi.softAPgetStationNum();
  doc["uptimeSec"]     = millis() / 1000;
  doc["freeHeap"]      = ESP.getFreeHeap();
  doc["natEnabled"]    = natEnabled;

  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleRestart() {
  if (!checkAuth()) return;
  server.send(200, "text/plain", "restarting");
  restartPending = true;
  restartAt = millis() + 800;
}

void handleFactoryReset() {
  if (!checkAuth()) return;
  LittleFS.remove(CONFIG_FILE);
  server.send(200, "text/plain", "reset-done");
  restartPending = true;
  restartAt = millis() + 800;
}

// ==================== وضع الإعداد ====================
void startSetupMode() {
  repeaterModeActive = false;
  ledMode = LED_SETUP;
  Serial.println("📡 وضع الإعداد: لا توجد إعدادات محفوظة (أو فشل الاتصال)");

  // بناء اسم شبكة الإعداد وكلمة سرها مرة واحدة: كود ثابت فريد لهذا الجهاز
  // (مبني على رقم الشريحة الفريد ESP.getChipId، فيبقى نفس الاسم/كلمة السر
  //  عبر كل عمليات إعادة التشغيل ولا يتغيّر أبدًا، وأيضًا يميّز كل جهاز عن
  //  غيره لو ركّبت أكثر من موزّع في نفس المكان. كلمة السر هنا تحمي شبكة
  //  الإعداد نفسها حتى لا يقدر أي شخص قريب يتصل بها ويغيّر إعداداتك)
  if (setupApSsid.length() == 0) {
    char codeBuf[7];
    snprintf(codeBuf, sizeof(codeBuf), "%06X", ESP.getChipId() & 0xFFFFFF);
    setupApSsid = String(DEFAULT_AP_NAME) + "-" + String(codeBuf);
    setupApPass = "wifi" + String(codeBuf); // 10 خانات ثابتة (يفي بشرط WPA2 الأدنى 8 خانات)
  }

  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(SETUP_AP_IP, SETUP_AP_IP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(setupApSsid.c_str(), setupApPass.c_str());

  Serial.print("اتصل بشبكة: "); Serial.println(setupApSsid);
  Serial.print("كلمة السر: ");  Serial.println(setupApPass);
  Serial.print("ثم افتح المتصفح على: http://"); Serial.println(WiFi.softAPIP());

  dnsServer.start(DNS_PORT, "*", SETUP_AP_IP);

  server.on("/", HTTP_GET, handleSetupRoot);
  server.on("/scan", HTTP_GET, handleScan);
  server.on("/save", HTTP_POST, handleSave);
  // روابط اكتشاف البوابة الأسيرة (Captive Portal) للأنظمة المختلفة
  server.on("/generate_204", HTTP_GET, handleSetupRoot);
  server.on("/gen_204", HTTP_GET, handleSetupRoot);
  server.on("/fwlink", HTTP_GET, handleSetupRoot);
  server.on("/hotspot-detect.html", HTTP_GET, handleSetupRoot);
  server.on("/ncsi.txt", HTTP_GET, handleSetupRoot);
  server.on("/connecttest.txt", HTTP_GET, handleSetupRoot);
  server.onNotFound(handleSetupRoot);
  server.begin();

  Serial.println("✅ خادم الإعداد جاهز");
}

// ==================== وضع التوزيع (Repeater) ====================
void startRepeaterMode() {
  Serial.print("🔌 محاولة الاتصال بالراوتر: "); Serial.println(routerSSID);
  ledMode = LED_CONNECTING;

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.begin(routerSSID.c_str(), routerPASS.c_str());

  uint8_t tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 40) { // ~20 ثانية
    delay(500);
    Serial.print(".");
    tries++;
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("❌ تعذّر الاتصال بالراوتر خلال المهلة، إعادة فتح وضع الإعداد");
    ledMode = LED_ERROR;
    startSetupMode();
    return;
  }

  Serial.print("✅ تم الاتصال بالراوتر، IP: "); Serial.println(WiFi.localIP());

  WiFi.softAPConfig(REPEATER_AP_IP, REPEATER_AP_IP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(apName.c_str(), apPass.length() ? apPass.c_str() : NULL);
  Serial.print("📶 يبث الآن شبكة: "); Serial.println(apName);
  Serial.print("عنوان الشبكة الجديدة: "); Serial.println(WiFi.softAPIP());

#if NAPT_SUPPORTED
  WiFi.softAPDhcpServer().setDns(WiFi.dnsIP(0));
  err_t naptErr = ip_napt_init(NAPT_ENTRIES, NAPT_PORTS);
  if (naptErr == ERR_OK) {
    naptErr = ip_napt_enable_no(SOFTAP_IF, 1);
  }
  natEnabled = (naptErr == ERR_OK);
  Serial.println(natEnabled ? "✅ توزيع الإنترنت (NAT) مفعّل بنجاح" : "❌ فشل تفعيل NAT");
#else
  natEnabled = false;
  Serial.println("⚠️ هذا الإصدار غير مُعدّ بدعم NAPT.");
  Serial.println("   غيّر من Arduino IDE: Tools > LwIP Variant إلى 'v2 Higher Bandwidth'");
#endif

  if (MDNS.begin("esprepeater")) {
    MDNS.addService("http", "tcp", 80);
    Serial.println("🌐 يمكنك أيضًا فتح: http://esprepeater.local");
  }

  server.on("/", HTTP_GET, handleStatus);
  server.on("/api/status", HTTP_GET, handleApiStatus);
  server.on("/restart", HTTP_POST, handleRestart);
  server.on("/factory-reset", HTTP_POST, handleFactoryReset);
  server.onNotFound(handleStatus);
  server.begin();

  repeaterModeActive = true;
  ledMode = LED_CONNECTED;
  reconnectFails = 0;
  Serial.println("✅ الجهاز جاهز ويعمل كموزّع إنترنت");
  Serial.println("   (اسم المستخدم للوحة التحكم: admin — كلمة المرور: كلمة سر شبكتك الجديدة)");
}

// ==================== خوارزمية إعادة الاتصال + المراقبة ====================
void checkStaConnection() {
  if (millis() - lastStaCheck < 10000) return;
  lastStaCheck = millis();

  if (WiFi.status() != WL_CONNECTED) {
    reconnectFails++;
    Serial.printf("⚠️ انقطع الاتصال بالراوتر، محاولة رقم %d/%d\n", reconnectFails, MAX_RECONNECT_FAILS);
    ledMode = LED_ERROR;
    WiFi.reconnect();
    if (reconnectFails >= MAX_RECONNECT_FAILS) {
      Serial.println("🔁 فشل متكرر، إعادة تشغيل الجهاز بالكامل كإجراء احترازي...");
      delay(300);
      ESP.restart();
    }
  } else {
    if (reconnectFails > 0) Serial.println("✅ عاد الاتصال بالراوتر");
    reconnectFails = 0;
    ledMode = LED_CONNECTED;
  }
}

// ==================== زر إعادة الضبط ====================
void handleResetButton() {
  if (digitalRead(RESET_BUTTON_PIN) == LOW) {
    if (!resetButtonHeld) {
      resetButtonHeld = true;
      resetHoldStart = millis();
    } else if (millis() - resetHoldStart >= 3000) {
      Serial.println("🔄 ضغط طويل مكتشف — إعادة ضبط المصنع...");
      LittleFS.remove(CONFIG_FILE);
      delay(300);
      ESP.restart();
    }
  } else {
    resetButtonHeld = false;
  }
}

// ==================== مؤشر LED ====================
void handleLed() {
  int interval;
  switch (ledMode) {
    case LED_CONNECTING: digitalWrite(LED_PIN, LOW);  return; // إضاءة ثابتة
    case LED_ERROR:       digitalWrite(LED_PIN, HIGH); return; // انطفاء
    case LED_SETUP:       interval = 1000; break;
    case LED_CONNECTED:   interval = 200;  break;
    default:              interval = 1000; break;
  }
  if (millis() - lastBlink >= (unsigned long)interval) {
    lastBlink = millis();
    ledState = !ledState;
    digitalWrite(LED_PIN, ledState ? LOW : HIGH);
  }
}

// ==================== Setup / Loop ====================
void setup() {
  Serial.begin(115200);
  delay(200);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH); // مطفأ مبدئيًا (Active LOW)
  pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);

  Serial.println();
  Serial.println("====================================");
  Serial.println("   ESP8266 WiFi Repeater — بدء التشغيل");
  Serial.println("====================================");

  if (!LittleFS.begin()) {
    Serial.println("⚠️ فشل تحميل نظام الملفات، إعادة التهيئة...");
    LittleFS.format();
    LittleFS.begin();
  }

  loadConfig();

  if (configExists) {
    startRepeaterMode();
  } else {
    startSetupMode();
  }
}

void loop() {
  handleResetButton();

  if (restartPending && millis() >= restartAt) {
    ESP.restart();
  }

  if (repeaterModeActive) {
    checkStaConnection();
    MDNS.update();
  } else {
    dnsServer.processNextRequest();
  }

  server.handleClient();
  handleLed();
}
