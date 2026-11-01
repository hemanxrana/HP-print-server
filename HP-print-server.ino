#include <Arduino.h>
#include "logger.h"
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include "usb_printer_backend.h"
#include "ipp_pcl3_service.h"
#include "system_diagnostics.h"
#include <esp_heap_caps.h>
#include "esp_task_wdt.h"
#include "config.h"

// ESP32-S3 USB-to-Wi-Fi print server.
// Network side: RAW JetDirect/AppSocket on TCP 9100 and a PCL3GUI-only IPP
// wrapper on TCP 631 (both stream verbatim into the USB Bulk OUT pipe).

WebServer configServer(WEB_SERVER_PORT);
Preferences preferences;
UsbHostManager usbHost;
UsbPrinterBackend usbPrinterBackend(usbHost);
IppPcl3Service ippService(usbPrinterBackend);

struct Config {
  String ssid;
  String password;
  bool staticMode = false;
  String staticIp;
  String staticGw;
  String staticSn;
};
Config config;
static unsigned long lastStatus = 0;
static bool mdnsReady = false;
static bool configApActive = false;
static uint32_t bootCount = 0;

String jsonEsc(String s) { s.replace("\\", "\\\\"); s.replace("\"", "\\\""); s.replace("\r", "\\r"); s.replace("\n", "\\n"); return s; }
void defaults() {
  config.ssid = "";
  config.password = "";
  config.staticMode = false;
  config.staticIp = "";
  config.staticGw = "";
  config.staticSn = "";
}

bool parseIPv4(const String &text, IPAddress &ip) {
  unsigned int a, b, c, d;
  char tail = 0;
  if (sscanf(text.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4) return false;
  if (a > 255 || b > 255 || c > 255 || d > 255) return false;
  ip = IPAddress(a, b, c, d);
  return true;
}

void applyStaticIp() {
  if (!config.staticMode) { WiFi.config(IPAddress(0, 0, 0, 0), IPAddress(0, 0, 0, 0), IPAddress(0, 0, 0, 0)); return; }
  IPAddress sip, sgw, ssn;
  if (!parseIPv4(config.staticIp, sip) || !parseIPv4(config.staticGw, sgw) || !parseIPv4(config.staticSn, ssn)) {
    Log.println("[WiFi] Saved static-IP settings are invalid; falling back to DHCP");
    config.staticMode = false;
    WiFi.config(IPAddress(0, 0, 0, 0), IPAddress(0, 0, 0, 0), IPAddress(0, 0, 0, 0));
    return;
  }
  WiFi.config(sip, sgw, ssn);
  Log.printf("[WiFi] Static IP: %s / %s / %s\n", config.staticIp.c_str(), config.staticGw.c_str(), config.staticSn.c_str());
}
void loadConfig() {
  defaults();
  if (!preferences.begin(NVS_NAMESPACE, true)) { Log.println("[CFG] Preferences read failed; using defaults"); return; }
  config.ssid = preferences.getString(NVS_KEY_SSID, config.ssid);
  config.password = preferences.getString(NVS_KEY_PASS, config.password);
  config.staticMode = preferences.getBool(NVS_KEY_STATIC_MODE, config.staticMode);
  config.staticIp = preferences.getString(NVS_KEY_IP, config.staticIp);
  config.staticGw = preferences.getString(NVS_KEY_GW, config.staticGw);
  config.staticSn = preferences.getString(NVS_KEY_SN, config.staticSn);
  preferences.end();
}
bool saveConfig() {
  if (!preferences.begin(NVS_NAMESPACE, false)) return false;
  const size_t ssidWritten = preferences.putString(NVS_KEY_SSID, config.ssid);
  const size_t passWritten = preferences.putString(NVS_KEY_PASS, config.password);
  const size_t ipWritten = preferences.putString(NVS_KEY_IP, config.staticIp);
  const size_t gwWritten = preferences.putString(NVS_KEY_GW, config.staticGw);
  const size_t snWritten = preferences.putString(NVS_KEY_SN, config.staticSn);
  const bool modeSaved = preferences.putBool(NVS_KEY_STATIC_MODE, config.staticMode);
  preferences.end();
  return (config.ssid.isEmpty() || ssidWritten > 0) && (config.password.isEmpty() || passWritten > 0) && modeSaved && (config.staticIp.isEmpty() || ipWritten > 0) && (config.staticGw.isEmpty() || gwWritten > 0) && (config.staticSn.isEmpty() || snWritten > 0);
}

bool startConfigAP() {
  WiFi.mode(WIFI_AP_STA); WiFi.setSleep(false); WiFi.setHostname("printer");
  if (!WiFi.softAP(AP_SSID, AP_PASSWORD, 1, false, 4)) { Log.println("[AP] Failed to start configuration AP"); configApActive = false; return false; }
  configApActive = true; Log.printf("[AP] SSID: %s\n", AP_SSID); Log.printf("[AP] Configure at http://%s\n", WiFi.softAPIP().toString().c_str()); return true;
}

void startRawDiscovery() {
  const uint32_t heapBefore = ESP.getFreeHeap();
  MDNS.end(); mdnsReady = false;
  if (!MDNS.begin("printer")) { Log.println("[mDNS] Failed to start printer.local discovery responder"); return; }
  mdnsReady = true;
  MDNS.setInstanceName(PRINTER_INSTANCE_NAME);
  bool ippOk = MDNS.addService("ipp", "tcp", IPP_PORT);
  bool rawOk = MDNS.addService("pdl-datastream", "tcp", RAW_SOCKET_PORT);
  if (ippOk) {
    MDNS.addServiceTxt("ipp", "tcp", "rp", "ipp/print");
    MDNS.addServiceTxt("ipp", "tcp", "txtvers", "1");
    MDNS.addServiceTxt("ipp", "tcp", "qtotal", "1");
    MDNS.addServiceTxt("ipp", "tcp", "device-id", PRINTER_DEVICE_ID);
    MDNS.addServiceTxt("ipp", "tcp", "pdl", "application/vnd.hp-PCL");
    MDNS.addServiceTxt("ipp", "tcp", "product", "(" PRINTER_MAKE_MODEL ")");
    MDNS.addServiceTxt("ipp", "tcp", "ty", PRINTER_MAKE_MODEL);
    MDNS.addServiceTxt("ipp", "tcp", "Color", "T");
    MDNS.addServiceTxt("ipp", "tcp", "Duplex", "F");
    MDNS.addServiceTxt("ipp", "tcp", "note", "ESP32-S3 PCL3GUI USB bridge");
    // Stable printer identity: same MAC-derived UUID the IPP endpoint reports,
    // so clients remember us across IP changes.
    MDNS.addServiceTxt("ipp", "tcp", "UUID", IppPcl3Service::printerUuidBare().c_str());
    MDNS.addServiceTxt("ipp", "tcp", "priority", "0");
    MDNS.addServiceTxt("ipp", "tcp", "air", "none");
    String adminUrl = String("http://") + activeIp() + "/";
    MDNS.addServiceTxt("ipp", "tcp", "adminurl", adminUrl.c_str());
    Log.println("[mDNS] _ipp._tcp (PCL3GUI-only) + printer.local advertised");
  }
  if (rawOk) {
    MDNS.addServiceTxt("pdl-datastream", "tcp", "pdl", PRINTER_DOCUMENT_FORMAT);
    Log.println("[mDNS] printer.local -> RAW 9100 advertised");
  }
  const uint32_t heapAfter = ESP.getFreeHeap();
  Log.printf("[mDNS] responder ready: heap %u -> %u bytes\n",
             (unsigned)heapBefore, (unsigned)heapAfter);
}

String printerStateText() { switch (usbPrinterBackend.state()) { case UsbPrinterBackend::OFFLINE: return "Offline"; case UsbPrinterBackend::IDLE: return "Ready"; case UsbPrinterBackend::PRINTING: return "Printing"; case UsbPrinterBackend::ERROR: return "Printer error"; } return "Unknown"; }
String printerReasonText() { const String &reason = usbPrinterBackend.statusReason(); if (reason == "waiting-for-usb-printer") return "Connect a compatible USB printer"; if (reason == "enumerating-usb-device") return "Checking the USB printer"; if (reason == "printer-interface-ready" || reason == "usb-printer-ready") return "Printer is ready"; if (reason == "raw-job-in-progress") return "Sending the print job"; if (reason == "raw-job-draining") return "Finishing the print job"; if (reason == "usb-printer-reports-paper-empty") return "Paper is empty or unavailable"; if (reason == "usb-printer-reports-not-selected") return "Printer is not selected or not ready"; if (reason == "usb-printer-reports-error" || reason == "usb-printer-reports-error-after-job") return "Printer reported an error"; if (reason == "selected-interface-has-no-bulk-output") return "No compatible RAW printing interface was found"; if (reason.startsWith("USB bulk transfer failed") || reason.startsWith("USB Bulk OUT")) return "USB transfer to the printer failed"; return reason.length() ? reason : "Waiting for printer"; }
String usbStatusText() { if (!usbHost.device().attached) return "No USB printer detected"; if (!usbHost.portStatusValid()) return "Printer connected; detailed status is not available yet"; if (usbHost.portStatusError()) return "Printer reports an error"; if (usbHost.portStatusPaperEmpty()) return "Paper is empty or unavailable"; if (!usbHost.portStatusSelected()) return "Printer is not selected or not ready"; return "Printer is selected and ready"; }
String wifiStatusText() { if (WiFi.status() == WL_CONNECTED) return String("Connected · ") + WiFi.localIP().toString(); if (configApActive) return String("Setup access point · ") + WiFi.softAPIP().toString(); return "Not connected"; }
String activeNetworkName() { if (WiFi.status() == WL_CONNECTED) return WiFi.SSID(); if (configApActive) return String(AP_SSID); return "Not connected"; }
String activeIp() { if (WiFi.status() == WL_CONNECTED) return WiFi.localIP().toString(); if (configApActive) return WiFi.softAPIP().toString(); return ""; }

static int lastWifiStatus = -1;
static unsigned long lastWifiAttemptMs = 0;

void sendJsonScan() { const int n = WiFi.scanNetworks(false, true); String out = "["; bool first = true; for (int i = 0; i < n; ++i) { String ssid = WiFi.SSID(i); if (ssid.isEmpty()) continue; if (!first) out += ","; first = false; out += String("{\"ssid\":\"") + jsonEsc(ssid) + "\",\"rssi\":" + String(WiFi.RSSI(i)) + ",\"channel\":" + String(WiFi.channel(i)) + "}"; } out += "]"; WiFi.scanDelete(); configServer.send(200, "application/json", out); }

void sendJsonStatus() {
  const bool printerAttached = usbHost.device().attached;
  const String printerName = printerAttached && usbHost.device().product.length() ? usbHost.device().product : "USB printer";
  String out; out.reserve(1600);
  out += "{\"printer\":\"" + jsonEsc(printerStateText());
  out += "\",\"printerReason\":\"" + jsonEsc(printerReasonText());
  out += "\",\"printerName\":\"" + jsonEsc(printerName);
  out += "\",\"usbStatus\":\"" + jsonEsc(usbStatusText());
  out += "\",\"wifi\":\"" + jsonEsc(wifiStatusText());
  out += "\",\"ssid\":\"" + jsonEsc(activeNetworkName());
  out += "\",\"rawConnected\":" + String(usbPrinterBackend.rawClientConnected() ? "true" : "false");
  out += ",\"ippActive\":" + String(ippService.active() ? "true" : "false") + ",\"ippLastError\":\"" + jsonEsc(ippService.lastError()) + "\"";
  out += ",\"usbAttached\":" + String(printerAttached ? "true" : "false");
  out += ",\"ip\":\"" + jsonEsc(activeIp());
  out += "\",\"mdnsReady\":" + String(mdnsReady ? "true" : "false");
  out += ",\"hostname\":\"" + String(mdnsReady ? "printer.local" : "") + "\"";
  out += ",\"jobsOk\":" + String(usbPrinterBackend.jobsCompleted()) + ",\"jobsFailed\":" + String(usbPrinterBackend.jobsFailed());
  out += ",\"maxDrainMs\":" + String(usbPrinterBackend.maxDrainMs()) + ",\"lastError\":\"" + jsonEsc(usbPrinterBackend.lastError()) + "\"";
  out += ",\"freeHeap\":" + String(ESP.getFreeHeap()) + ",\"boots\":" + String(bootCount) + ",\"usbWedged\":" + String(usbHost.bulkOutWedged() ? "true" : "false");
  out += ",\"netMode\":\"" + String(config.staticMode ? "static" : "dhcp") + "\"";
  out += ",\"staticIp\":\"" + jsonEsc(config.staticIp) + "\",\"staticGw\":\"" + jsonEsc(config.staticGw) + "\",\"staticSn\":\"" + jsonEsc(config.staticSn) + "\"";
  out += ",\"psramTotal\":" + String((unsigned)ESP.getPsramSize()) + ",\"psramFree\":" + String((unsigned)ESP.getFreePsram()) + "}";
  configServer.send(200, "application/json", out);
}

struct HtmlBuffer {
  char *data = nullptr;
  size_t length = 0;
  size_t capacity = 0;

  bool ensure() {
    if (data) return true;
    constexpr size_t cap = 16384;
    data = (char *)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!data) data = (char *)heap_caps_malloc(cap, MALLOC_CAP_8BIT);
    if (!data) return false;
    capacity = cap;
    data[0] = '\0';
    return true;
  }
  void clear() { length = 0; if (data) data[0] = '\0'; }
  void append(const char *text) {
    if (!data || !text) return;
    size_t n = strlen(text);
    if (length + n >= capacity) n = capacity - 1 - length;
    memcpy(data + length, text, n);
    length += n;
    data[length] = '\0';
  }
  void append(const String &text) { append(text.c_str()); }
  void appendEscaped(const String &text) {
    for (size_t i = 0; i < text.length(); ++i) {
      const char c = text[i];
      switch (c) {
        case '&': append("&amp;"); break;
        case '<': append("&lt;"); break;
        case '>': append("&gt;"); break;
        case 34: append("&quot;"); break;
        case 39: append("&#39;"); break;
        default: { char one[2] = {c, '\0'}; append(one); break; }
      }
    }
  }
  const char *c_str() const { return data ? data : ""; }
};

static HtmlBuffer dashboardBuffer;

const char *dashboard() {
  const bool printerAttached = usbHost.device().attached;
  const String deviceName = printerAttached && usbHost.device().product.length() ? usbHost.device().product : "No USB printer";
  const String ip = activeIp(); const String networkName = activeNetworkName();
  HtmlBuffer &out = dashboardBuffer;
  if (!out.ensure()) return "";
  out.clear();
  out.append(R"HTML(<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><meta name="theme-color" content="#0f6b7a"><title>HP Print Server</title><style>*{box-sizing:border-box;margin:0;padding:0}body{font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif;background:#f2f4f8;color:#1f2937;line-height:1.5}#app{max-width:860px;margin:0 auto;padding:18px}header{display:flex;align-items:center;justify-content:space-between;padding:14px 0;border-bottom:1px solid #e5e7eb;margin-bottom:20px}h1{font-size:18px;font-weight:700;color:#111827;display:flex;align-items:center;gap:7px}h1 .dot{width:7px;height:7px;background:#10b981;border-radius:50%;display:inline-block}.sub{font-size:12px;color:#6b7280;margin-top:2px}.tabs{display:flex;gap:4px;overflow-x:auto;scrollbar-width:none;margin-bottom:18px}.tabs a{flex-shrink:0;padding:8px 14px;border-radius:8px;text-decoration:none;color:#6b7280;font-weight:600;font-size:13px;border:1px solid #e5e7eb;background:#fff}.tabs a.active{background:#0f6b7a;color:#fff;border-color:#0f6b7a}.tabs a:hover:not(.active){background:#f3f4f6}.cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:12px;margin-bottom:18px}.card{background:#fff;border:1px solid #e5e7eb;border-radius:10px;padding:15px}.card h3{font-size:11px;text-transform:uppercase;letter-spacing:.05em;color:#9ca3af;margin-bottom:8px;font-weight:700}.big{font-size:18px;font-weight:700;color:#111827}.ok{color:#10b981}.warn{color:#f59e0b}.muted{color:#6b7280;font-size:12.5px}section{margin-bottom:22px}section h2{font-size:14px;font-weight:700;margin-bottom:11px;color:#111827}.form{background:#fff;border:1px solid #e5e7eb;border-radius:10px;padding:15px}.field{margin-bottom:11px}.field label{display:block;font-size:12px;font-weight:600;color:#374151;margin-bottom:5px}.field input,.field select{width:100%;padding:9px 11px;border:1px solid #d1d5db;border-radius:8px;background:#fff;color:#111827;font-size:13px}.field input,.field select{width:100%;padding:9px 11px;border:1px solid #d1d5db;border-radius:8px;background:#fff;color:#111827;font-size:13px}.check{display:flex;align-items:center;gap:8px;margin-bottom:12px}.check input{width:auto}.btn{display:inline-flex;align-items:center;justify-content:center;padding:9px 16px;border-radius:8px;border:1px solid transparent;text-decoration:none;font-size:13px;font-weight:600;cursor:pointer}.btn.primary{background:#0f6b7a;color:#fff}.btn.primary:hover{background:#0d5a67}.btn.secondary{background:#fff;color:#374151;border-color:#d1d5db}.btn.secondary:hover{background:#f3f4f6}.btn.small{padding:6px 12px;font-size:12px}.msg{padding:9px 12px;border-radius:8px;margin-bottom:11px}.msg.muted{background:#f3f4f6;color:#374151;border:1px solid #e5e7eb}#refresh{font-size:11.5px;color:#6b7280}@media(max-width:640px){#app{padding:12px}}</style></head><body><div id="app"><header><h1><span class="dot"></span> HP Print Server</h1><span id="refresh">&ndash;</span></header><nav class="tabs"><a class="active" href="#dash">Dashboard</a><a href="#net">Network</a></nav><div class="cards"><div class="card"><h3>USB Printer</h3><div class="big" id="cP">--</div><div class="muted" id="cPR">--</div></div><div class="card"><h3>Wi-Fi</h3><div class="big" id="cN">--</div><div class="muted" id="cNI">--</div></div><div class="card"><h3>USB</h3><div class="big" id="cU">--</div><div class="muted" id="cUD">--</div></div></div>

<section id="dash"><h2>Status</h2><div class="form"><div class="field"><label>USB Printer</label><div class="muted" id="sPrinter">--</div></div><div class="field"><label>State</label><div class="muted" id="sState">--</div></div><div class="field"><label>RAW Client</label><div class="muted" id="sRaw">--</div></div><div class="field"><label>Wi-Fi</label><div class="muted" id="sWifi">--</div></div><div class="field"><label>IP Address</label><div class="muted" id="sIp">--</div></div><div class="field"><label>mDNS</label><div class="muted" id="sMdns">--</div></div><div class="field"><label>Free Heap</label><div class="muted" id="sHeap">--</div></div><div class="field"><label>PSRAM</label><div class="muted" id="sPsram">--</div></div></div></section>

<section id="net"><h2>Wi-Fi</h2><div class="form"><div class="field"><label>Network name</label><select id="ssidSel"><option value="">Select a network&hellip;</option></select></div><div class="field"><label>Password</label><input type="password" id="passIn" placeholder="Password (leave empty to keep saved)"></div><div class="field"><button class="btn secondary small" id="scanBtn" type="button" onclick="scanWifi()">Scan networks</button>&nbsp;<button class="btn primary" type="button" onclick="saveWifi()">Connect &amp; Reboot</button></div><div class="msg muted" id="wifiMsg">Saved Wi-Fi re-connects automatically at boot.</div></div><h2>IP Address</h2><div class="form"><div class="field"><label>Address mode</label><select id="ipMode" onchange="ipModeUi()"><option value="dhcp">DHCP (automatic)</option><option value="static">Static IP</option></select></div><div class="field"><label>IP address</label><input type="text" id="ipId" placeholder="192.168.1.111" maxlength="15"></div><div class="field"><label>Gateway</label><input type="text" id="gwId" placeholder="192.168.1.1" maxlength="15"></div><div class="field"><label>Subnet mask</label><input type="text" id="snId" placeholder="255.255.255.0" maxlength="15"></div><div class="field"><button class="btn primary" type="button" onclick="saveNet()">Save IP &amp; Reboot</button></div><div class="msg muted" id="netMsg">DHCP is the default; choose Static to pin the address you print to.</div></div></section>

<script>const $=id=>document.getElementById(id);function esc(t){const d=document.createElement('div');d.textContent=t;return d.innerHTML}async function scanWifi(){const b=$('scanBtn'),m=$('wifiMsg');b.disabled=true;m.textContent='Scanning...';try{const r=await fetch('/scan.json?ts='+Date.now());const a=await r.json();const sel=$('ssidSel');sel.innerHTML='<option value="">Select a network&hellip;</option>';a.sort((x,y)=>y.rssi-x.rssi);a.forEach(x=>{const o=document.createElement('option');o.value=esc(x.ssid);o.textContent=esc(x.ssid)+' ('+x.rssi+' dBm)';sel.appendChild(o);});m.textContent='Found '+a.length+'.';}catch(e){m.textContent='Scan failed.';}finally{b.disabled=false;}}let netPrefilled=false;function ipModeUi(){const st=document.getElementById('ipMode').value==='static';['ipId','gwId','snId'].forEach(i=>document.getElementById(i).disabled=!st);}
async function saveWifi(){const m=document.getElementById('wifiMsg');const sel=document.getElementById('ssidSel');if(!sel.value){m.textContent='Select a network first.';return;}m.textContent='Saving Wi-Fi…';const f=new URLSearchParams();f.append('ssid',sel.value);f.append('password',document.getElementById('passIn').value);try{const r=await fetch('/save?ts='+Date.now(),{method:'POST',body:f});m.textContent=await r.text();}catch(e){m.textContent='Save failed.';}}
async function saveNet(){const m=document.getElementById('netMsg');m.textContent='Saving IP settings…';const f=new URLSearchParams();f.append('ipMode',document.getElementById('ipMode').value);f.append('staticIp',document.getElementById('ipId').value);f.append('staticGw',document.getElementById('gwId').value);f.append('staticSn',document.getElementById('snId').value);try{const r=await fetch('/net/save?ts='+Date.now(),{method:'POST',body:f});m.textContent=await r.text();}catch(e){m.textContent='Save failed.';}}
async function refresh(){try{const r=await fetch('/status.json?ts='+Date.now());const s=await r.json();if(!netPrefilled){document.getElementById('ipMode').value=s.netMode||'dhcp';document.getElementById('ipId').value=s.staticIp||'';document.getElementById('gwId').value=s.staticGw||'';document.getElementById('snId').value=s.staticSn||'';ipModeUi();netPrefilled=true;}$('cP').textContent=s.usbAttached?s.printerName:'No USB Printer';$('cPR').textContent=s.printerReason;$('cP').className='big '+(s.usbAttached?'ok':'muted');$('cN').textContent=s.wifi;$('cNI').textContent=s.ip||'&ndash;';$('cU').textContent=s.usbStatus;$('cUD').textContent=s.usbWedged?'Wedged':'OK';$('cUD').className='muted '+(s.usbWedged?'warn':'');$('sPrinter').textContent=s.usbAttached?s.printerName:'Not attached';$('sState').textContent=s.printer;$('sRaw').textContent=s.rawConnected?'Connected':'Idle';$('sWifi').textContent=s.wifi;$('sIp').textContent=s.ip||'&ndash;';$('sMdns').textContent=s.mdnsReady?'printer.local':'&ndash;';$('sHeap').textContent=(s.freeHeap/1024).toFixed(0)+' KB';$('sPsram').textContent=(s.psramFree/1024).toFixed(0)+' / '+(s.psramTotal/1024).toFixed(0)+' KB';$('refresh').textContent='Updated '+new Date().toLocaleTimeString();}catch(e){}}setInterval(refresh,5000);refresh();</script></div></body></html>)HTML");
  return out.c_str();
}

void handleRoot() { const char *page = dashboard(); const size_t length = strlen(page); configServer.setContentLength(length); configServer.send(200, "text/html; charset=utf-8"); if (length) configServer.sendContent(page, length); }

void handleSave() {
  if (!configServer.hasArg("ssid") || configServer.arg("ssid").isEmpty()) {
    configServer.send(400, "text/plain; charset=utf-8", "Wi-Fi network name is required.");
    return;
  }
  config.ssid = configServer.arg("ssid");
  if (configServer.hasArg("clearPassword")) config.password = "";
  else if (configServer.hasArg("password") && !configServer.arg("password").isEmpty()) config.password = configServer.arg("password");
  if (!saveConfig()) {
    configServer.send(500, "text/plain; charset=utf-8", "Could not save Wi-Fi settings. The device was not restarted.");
    return;
  }
  if (configServer.hasArg("reboot")) {
    configServer.send(200, "text/html; charset=utf-8", "<p>Rebooting…</p>");
    delay(200);
    ESP.restart();
    return;
  }
  configServer.send(200, "text/html; charset=utf-8", "<p>Wi-Fi settings saved. Rebooting…</p>");
  delay(300);
  ESP.restart();
}

void handleSaveNet() {
  const bool wantsStatic = configServer.arg("ipMode") == String("static");
  if (wantsStatic) {
    const String ip = configServer.arg("staticIp");
    const String gw = configServer.arg("staticGw");
    const String sn = configServer.arg("staticSn");
    IPAddress a, b, c;
    if (!parseIPv4(ip, a) || !parseIPv4(gw, b) || !parseIPv4(sn, c)) {
      configServer.send(400, "text/plain; charset=utf-8", "Static IP, gateway and subnet must all be valid IPv4 addresses.");
      return;
    }
    config.staticIp = ip;
    config.staticGw = gw;
    config.staticSn = sn;
  }
  config.staticMode = wantsStatic;
  if (!saveConfig()) {
    configServer.send(500, "text/plain; charset=utf-8", "Could not save network settings.");
    return;
  }
  configServer.send(200, "text/html; charset=utf-8", "<p>Network settings saved. Rebooting to apply…</p>");
  delay(200);
  ESP.restart();
}

void onWifiConnected() {
  if (configApActive) {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    configApActive = false;
    Log.println("[AP] Station link up; configuration AP stopped");
  }
  startRawDiscovery();
  usbPrinterBackend.restartRawListener();
  ippService.rebind();
  Log.println("[WiFi] Station link ready; RAW and IPP listeners re-bound");
}

void serviceWifi() {
  // mDNS responder health: if it ever failed to come up (begin() failure after
  // a reconnect, OOM, ...), retry periodically instead of staying permanently
  // undiscoverable. Healthy state is never churned.
  static unsigned long mdnsRetryDueMs = 0;
  if (!mdnsReady && millis() - mdnsRetryDueMs >= 3600000UL) {
    mdnsRetryDueMs = millis();
    Log.printf("[mDNS] responder down; retrying discovery (free heap %u)\n", (unsigned)ESP.getFreeHeap());
    startRawDiscovery();
    if (mdnsReady) usbPrinterBackend.restartRawListener();
  }

  const int status = (int)WiFi.status();
  if (status == WL_CONNECTED && lastWifiStatus != WL_CONNECTED) {
    onWifiConnected();
  } else if (status != WL_CONNECTED && lastWifiStatus == WL_CONNECTED) {
    Log.println("[WiFi] Station link lost; retrying in background");
  }

  if (status != WL_CONNECTED && !config.ssid.isEmpty() && millis() - lastWifiAttemptMs >= 30000UL) {
    lastWifiAttemptMs = millis();
    Log.printf("[WiFi] Retrying saved network %s\n", config.ssid.c_str());
    applyStaticIp();
    WiFi.reconnect();
  }
  lastWifiStatus = status;
}

void loadBootCount() {
  if (!preferences.begin("hp-diag", false)) return;
  bootCount = preferences.getUInt("boots", 0) + 1;
  preferences.putUInt("boots", bootCount);
  preferences.end();
}

void startWatchdog() {
  esp_task_wdt_config_t cfg = {};
  cfg.timeout_ms = WDT_TIMEOUT_SEC * 1000;
  cfg.idle_core_mask = 0;
  cfg.trigger_panic = true;
  esp_err_t err = esp_task_wdt_reconfigure(&cfg);
  if (err == ESP_ERR_INVALID_STATE) err = esp_task_wdt_init(&cfg);
  if (err != ESP_OK) {
    Log.printf("[WDT] Watchdog setup failed: %d; continuing without a task watchdog\n", (int)err);
    return;
  }
  esp_task_wdt_add(nullptr);
  Log.printf("[WDT] Task watchdog armed (%lu s)\n", (unsigned long)(WDT_TIMEOUT_SEC));
}

void setup() {
  Log.begin(115200); delay(500); Log.println(); SystemDiagnostics::logBoot(); Log.println("=== ESP32-S3 HP Print Server ===");
  loadBootCount();
  startWatchdog();
  Log.printf("[MEM] internal-heap-free=%lu psram-total=%u psram-free=%u\n",
                (unsigned long)ESP.getFreeHeap(), (unsigned)ESP.getPsramSize(),
                (unsigned)ESP.getFreePsram());
  Log.println("[MODE] RAW 9100 (JetDirect/AppSocket) + PCL3GUI-only IPP 631");
  loadConfig(); 
  bool wifiOk = false;
  if (!config.ssid.isEmpty()) {
    WiFi.mode(WIFI_STA); 
    WiFi.setSleep(false); 
    WiFi.setHostname("printer");
    applyStaticIp();
    Log.printf("[WiFi] Connecting to %s (%s)\n", config.ssid.c_str(), config.staticMode ? (String("static ") + config.staticIp).c_str() : String("DHCP").c_str());
    const unsigned long started = millis(); 
    while (WiFi.status() != WL_CONNECTED && millis() - started < 20000UL) { delay(250); Log.print('.'); } 
    Log.println();
    if (WiFi.status() == WL_CONNECTED) { wifiOk = true; Log.printf("[WiFi] Connected: %s\n", WiFi.localIP().toString().c_str()); }
  }
  if (!wifiOk) { startConfigAP(); }
  lastWifiStatus = (int)WiFi.status();
  if (WiFi.status() == WL_CONNECTED) {
    startRawDiscovery();
    usbPrinterBackend.restartRawListener();
  }
  usbPrinterBackend.begin();
  ippService.begin();
  configServer.on("/", HTTP_GET, handleRoot);
  configServer.on("/scan.json", HTTP_GET, sendJsonScan);
  configServer.on("/status.json", HTTP_GET, sendJsonStatus);
  configServer.on("/save", HTTP_POST, handleSave);
  configServer.on("/net/save", HTTP_POST, handleSaveNet);
  configServer.begin();
  Log.println("[HTTP] Configuration server ready");
  Log.printf("[HTTP] Open http://%s\n", mdnsReady ? "printer.local" : (activeIp().length() ? activeIp().c_str() : "device-address"));
  Log.println("[RAW] TCP 9100 server enabled");
}

void loop() {
  esp_task_wdt_reset();
  serviceWifi();
  configServer.handleClient();
  usbHost.poll();
  usbPrinterBackend.poll();
  ippService.poll();
  if (millis() - lastStatus > 1000) {
    lastStatus = millis();
    const int wifiState = (int)WiFi.status();
    const String ip = activeIp();
    const int usbState = (int)usbHost.state();
    const String printerState = printerStateText();
    const bool rawConnected = usbPrinterBackend.rawClientConnected();
    const bool ippActive = ippService.active();
    const bool usbStatusValid = usbHost.portStatusValid();
    const uint8_t usbPort = usbStatusValid ? usbHost.portStatusValue() : 0;
    static bool initialized = false;
    static int lastWifiState = -1;
    static String lastIp;
    static int lastUsbState = -1;
    static String lastPrinterState;
    static bool lastRawConnected = false;
    static bool lastIppActive = false;
    static bool lastUsbStatusValid = false;
    static uint8_t lastUsbPort = 0;
    const bool changed = !initialized || wifiState != lastWifiState || ip != lastIp || usbState != lastUsbState || printerState != lastPrinterState || rawConnected != lastRawConnected || ippActive != lastIppActive || usbStatusValid != lastUsbStatusValid || (usbStatusValid && usbPort != lastUsbPort);
    if (changed) {
      Log.printf("[STATUS] WiFi=%d IP=%s USB=%d printer=%s raw=%s ipp=%s%s\n",
                  wifiState, ip.c_str(), usbState, printerState.c_str(),
                  rawConnected ? "connected" : "idle",
                  ippActive ? "active" : "idle",
                  usbStatusValid ? (String(" usbport=0x") + String(usbPort, HEX)).c_str() : " usbstatus=unavailable");
      lastWifiState = wifiState;
      lastIp = ip;
      lastUsbState = usbState;
      lastPrinterState = printerState;
      lastRawConnected = rawConnected;
      lastIppActive = ippActive;
      lastUsbStatusValid = usbStatusValid;
      lastUsbPort = usbPort;
      initialized = true;
    }
  }
}
