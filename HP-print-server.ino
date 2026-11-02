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
#include <nvs_flash.h>
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
}

void loadConfig() {
  defaults();
  if (!preferences.begin(NVS_NAMESPACE, false)) { Log.println("[CFG] NVS open failed; using defaults"); return; }
  config.ssid = preferences.getString(NVS_KEY_SSID, config.ssid);
  config.password = preferences.getString(NVS_KEY_PASS, config.password);
  preferences.end();
}
bool saveConfig() {
  if (!preferences.begin(NVS_NAMESPACE, false)) {
    Log.printf("[CFG] NVS open for write FAILED; save aborted (freeHeap=%u)\n", (unsigned)ESP.getFreeHeap());
    return false;
  }
  const size_t ssidWritten = preferences.putString(NVS_KEY_SSID, config.ssid);
  const size_t passWritten = preferences.putString(NVS_KEY_PASS, config.password);
  preferences.end();
  Log.printf("[CFG] NVS save: ssid=%u pass=%u\n", (unsigned)ssidWritten, (unsigned)passWritten);
  return (config.ssid.isEmpty() || ssidWritten > 0) && (config.password.isEmpty() || passWritten > 0);
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
    MDNS.addServiceTxt("ipp", "tcp", "pdl", PRINTER_FORMAT_LIST_IPP);
    // "am" lists the advertised print modes; HP print apps display the dialect for the queue.
    MDNS.addServiceTxt("ipp", "tcp", "am", PRINTER_DOCUMENT_VERSION);
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
    MDNS.addServiceTxt("pdl-datastream", "tcp", "pdl", PRINTER_FORMAT_LIST_RAW);
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
static unsigned long lastDisconnectMs = 0;

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
  out += ",\"freeHeap\":" + String(ESP.getFreeHeap()) + ",\"heapTotal\":" + String(ESP.getHeapSize()) + ",\"boots\":" + String(bootCount) + ",\"usbWedged\":" + String(usbHost.bulkOutWedged() ? "true" : "false");
  out += ",\"psramTotal\":" + String((unsigned)ESP.getPsramSize()) + ",\"psramFree\":" + String((unsigned)ESP.getFreePsram()) + "}";
  configServer.send(200, "application/json", out);
}

struct HtmlBuffer {
  char *data = nullptr;
  size_t length = 0;
  size_t capacity = 0;

  bool ensure() {
    if (data) return true;
    constexpr size_t cap = 24576;
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
  out.append(R"HTML(<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><meta name="theme-color" content="#0f6b7a"><title>HP Print Server</title>
<style>
:root{--bg:#eef1f6;--card:#fff;--line:#e3e8ef;--ink:#0f172a;--muted:#64748b;--soft:#f1f5f9;--accent:#0f6b7a;--hover:#0d5a67;--ok:#10b981;--okink:#047857;--okbg:#ecfdf5;--warn:#f59e0b;--warnink:#92400e;--radius:14px;--shadow:0 1px 2px rgba(15,23,42,.05),0 10px 30px rgba(15,23,42,.06)}
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:system-ui,-apple-system,'Segoe UI',Roboto,sans-serif;background:linear-gradient(180deg,#e8edf4,#f2f5f9 260px);color:var(--ink);line-height:1.55;-webkit-font-smoothing:antialiased}
#app{max-width:1000px;margin:0 auto;padding:20px 18px 44px}
header{display:flex;align-items:center;justify-content:space-between;gap:12px;flex-wrap:wrap;padding:16px 0 15px;border-bottom:1px solid var(--line);margin-bottom:16px}
.brand{display:flex;align-items:center;gap:11px}
.logo{width:38px;height:38px;border-radius:11px;background:linear-gradient(135deg,#0f6b7a,#0891b2);color:#fff;display:flex;align-items:center;justify-content:center;font-weight:800;font-size:14px;box-shadow:0 4px 10px rgba(15,107,122,.35)}
h1{font-size:17px;font-weight:750;letter-spacing:-.01em}
.sub{font-size:12px;color:var(--muted)}
.hmeta{text-align:right}
#netState{font-size:12px;font-weight:650;color:#334155}
#updated{font-size:11.5px;color:var(--muted)}
nav.tabs{display:flex;gap:6px;overflow-x:auto;scrollbar-width:none;margin:0 0 18px}
nav.tabs a{padding:8px 16px;border-radius:999px;text-decoration:none;color:var(--muted);font-weight:650;font-size:12.5px;background:#fff;border:1px solid var(--line)}
nav.tabs a.active{background:var(--accent);border-color:var(--accent);color:#fff}
section{margin-bottom:20px}
h2.sec{font-size:12px;font-weight:750;text-transform:uppercase;letter-spacing:.07em;color:var(--muted);margin:0 0 10px 2px}
.cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(215px,1fr));gap:12px}
.card,.panel{background:var(--card);border:1px solid var(--line);border-radius:var(--radius);box-shadow:var(--shadow)}
.card{padding:16px}
.lbl{font-size:11px;font-weight:750;text-transform:uppercase;letter-spacing:.06em;color:#94a3b8;margin-bottom:8px}
.big{font-size:17px;font-weight:700;word-break:break-word}
.note{font-size:12px;color:var(--muted);margin-top:6px}
.off{color:var(--muted)}.warn{color:var(--warnink)}.ok{color:var(--okink)}
.panel{padding:16px 18px}
.row{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:9px 2px;border-bottom:1px solid var(--soft)}
.row:last-child{border-bottom:none}
.k{font-size:12.5px;color:var(--muted);font-weight:600;flex-shrink:0}
.v{font-size:13px;font-weight:600;text-align:right;word-break:break-all}
.vcode{font-family:ui-monospace,Consolas,monospace;font-size:11.5px;background:var(--soft);border:1px solid var(--line);color:#334155;padding:3px 9px;border-radius:7px}
.dot{width:8px;height:8px;border-radius:50%;background:#cbd5e1;display:inline-block;margin-right:7px}
.dot.on{background:var(--ok);box-shadow:0 0 0 3px var(--okbg)}
.dot.warn{background:var(--warn);box-shadow:0 0 0 3px #fef3c7}
.bar{height:6px;background:var(--soft);border-radius:999px;overflow:hidden;margin-top:7px}
.bar i{display:block;height:100%;border-radius:999px;width:0;background:linear-gradient(90deg,#0f6b7a,#0891b2);transition:width .5s ease}
.bar.low i{background:linear-gradient(90deg,#f59e0b,#fbbf24)}
.grid2{display:grid;grid-template-columns:1fr 1fr;gap:14px}
@media(max-width:720px){.grid2{grid-template-columns:1fr}.hmeta{text-align:left}}
.hint{font-size:12px;color:var(--muted);margin-top:10px}
.hint b{color:#334155}
.field{display:flex;flex-direction:column;gap:6px;margin-bottom:12px}
.field label{font-size:12px;font-weight:650;color:#334155}
.field input,.field select{width:100%;padding:10px 12px;border:1px solid #d3d9e2;border-radius:10px;background:#fff;color:var(--ink);font-size:13.5px;outline:none;transition:border .15s,box-shadow .15s}
.field input:focus,.field select:focus{border-color:var(--accent);box-shadow:0 0 0 3px rgba(15,107,122,.12)}
.btn{display:inline-flex;align-items:center;padding:10px 16px;border-radius:10px;border:1px solid transparent;font-size:13px;font-weight:650;cursor:pointer;transition:background .15s}
.btn.primary{background:var(--accent);color:#fff}
.btn.primary:hover{background:var(--hover)}
.btn.secondary{background:#fff;color:#334155;border-color:#d3d9e2}
.btn.secondary:hover{background:var(--soft)}
.btn.small{padding:8px 14px;font-size:12px}
.btnrow{display:flex;gap:9px;flex-wrap:wrap;margin-top:2px}
.msg{padding:9px 13px;border-radius:10px;font-size:12.5px;background:var(--soft);color:#334155;border:1px solid var(--line);margin-top:12px}
</style></head><body>
<div id="app">
<header>
<div class="brand"><div class="logo">HP</div><div><h1>HP Print Server</h1><div class="sub">Smart Tank 520 emulation &middot; RAW 9100 &middot; IPP 631</div></div></div>
<div class="hmeta"><div id="netState"><span class="dot" id="netDot"></span><span id="netLabel">starting&hellip;</span></div><div id="updated" style="margin-top:2px">&ndash;</div></div>
</header>
<nav class="tabs"><a class="active" href="#overview">Overview</a><a href="#system">System</a><a href="#wifi">Wi-Fi</a></nav>
<section id="overview"><h2 class="sec">Overview</h2><div class="cards">
<div class="card"><div class="lbl">USB Printer</div><div class="big" id="ovPrinter">--</div><div class="note" id="ovPrinterSub">--</div></div>
<div class="card"><div class="lbl">Network</div><div class="big" id="ovNet">--</div><div class="note" id="ovNetSub">--</div></div>
<div class="card"><div class="lbl">USB Link</div><div class="big" id="ovUsb">--</div><div class="note" id="ovUsbSub">--</div></div>
<div class="card"><div class="lbl">Jobs</div><div class="big" id="ovJobs">--</div><div class="note" id="ovJobsSub">--</div></div>
</div></section>
<section id="system"><h2 class="sec">System</h2><div class="grid2">
<div class="panel">
<div class="row"><span class="k">Printer state</span><span class="v" id="sysState">--</span></div>
<div class="row"><span class="k">IPP service</span><span class="v" id="sysIpp">--</span></div>
<div class="row"><span class="k">RAW socket</span><span class="v" id="sysRaw">--</span></div>
<div class="row"><span class="k">mDNS</span><span class="v" id="sysMdns">--</span></div>
<div class="row"><span class="k">Hostname</span><span class="v" id="sysHost">--</span></div>
<div class="row"><span class="k">IPP last error</span><span class="v" id="sysIppErr" style="font-size:11.5px;font-weight:500">--</span></div>
<div class="row"><span class="k">USB error log</span><span class="v" id="sysUsbErr" style="font-size:11.5px;font-weight:500">--</span></div>
</div>
<div class="panel">
<div class="row"><span class="k">Free heap</span><span class="v" id="sysHeap">--</span></div><div class="bar" id="heapBarWrap"><i id="heapBar"></i></div>
<div class="row" style="margin-top:14px"><span class="k">Free PSRAM</span><span class="v" id="sysPsram">--</span></div><div class="bar" id="psramBarWrap"><i id="psramBar"></i></div>
<h2 class="sec" style="margin:18px 0 6px">How to print</h2>
<div class="row"><span class="k">IPP printer URL</span><span class="vcode" id="chipIpp">--</span></div>
<div class="row"><span class="k">RAW (JetDirect)</span><span class="vcode" id="chipRaw">--</span></div>
<div class="hint">Add the printer in Windows as an <b>IPP device</b> using the URL above, or create a <b>Standard TCP/IP port</b> for the RAW address. Use an HP Smart Tank 520 driver &mdash; PCL3GUI output only.</div>
</div>
</div></section>
<section id="wifi"><h2 class="sec">Wi-Fi settings</h2><div class="panel">
<div class="field"><label>Network name</label><select id="ssidSel"><option value="">Select a network&hellip;</option></select></div>
<div class="field"><label>Password</label><input type="password" id="passIn" placeholder="Leave empty to keep the saved one" autocomplete="new-password"></div>
<div class="btnrow"><button class="btn secondary small" id="scanBtn" type="button" onclick="scanWifi()">Scan networks</button><button class="btn primary" type="button" onclick="saveWifi()">Connect &amp; reboot</button></div>
<div class="msg" id="wifiMsg">Saved Wi-Fi re-connects automatically at boot.</div>
</div></section>
<script>
const $=id=>document.getElementById(id);
function esc(t){const d=document.createElement('div');d.textContent=t;return d.innerHTML}
async function scanWifi(){const b=$('scanBtn'),m=$('wifiMsg');b.disabled=true;b.textContent='Scanning\u2026';try{const r=await fetch('/scan.json?ts='+Date.now());const a=await r.json();const sel=$('ssidSel');sel.innerHTML='<option value="">Select a network&hellip;</option>';a.sort((x,y)=>y.rssi-x.rssi);a.forEach(x=>{const o=document.createElement('option');o.value=esc(x.ssid);o.textContent=esc(x.ssid)+' ('+x.rssi+' dBm)';sel.appendChild(o);});m.textContent='Found '+a.length+' network'+(a.length===1?'':'s')+'.';}catch(e){m.textContent='Scan failed.';}finally{b.disabled=false;b.textContent='Scan networks';}}
async function saveWifi(){const m=$('wifiMsg');const sel=$('ssidSel');if(!sel.value){m.textContent='Select a network first.';return;}m.textContent='Saving Wi-Fi settings\u2026';const f=new URLSearchParams();f.append('ssid',sel.value);f.append('password',$('passIn').value);try{const r=await fetch('/save?ts='+Date.now(),{method:'POST',body:f});m.textContent=await r.text();}catch(e){m.textContent='Save failed.';}}
function bar(id,wrapId,frac,base){const w=$(wrapId);const p=Math.max(0,Math.min(100,frac*100));$(id).style.width=p+'%';w.className='bar'+(base>0&&frac<0.15?' low':'');}
function dots(el,text,on,warn){el.innerHTML='<span class="dot '+(warn?'warn':(on?'on':'off'))+'"></span>'+text;}
async function refresh(){try{const r=await fetch('/status.json?ts='+Date.now());const s=await r.json();
const attached=s.usbAttached;
$('ovPrinter').textContent=attached?s.printerName:'Not attached';
$('ovPrinter').className='big'+(attached?'':' off');
const ps=$('ovPrinterSub');ps.textContent=(s.printer||'--')+(s.printerReason?' \u00b7 '+s.printerReason:'');ps.className='note'+(attached?'':' off');
$('ovNet').textContent=s.ssid||s.wifi||'--';
$('ovNet').className='big'+(s.ssid?'':' off');
$('ovNetSub').textContent=(s.ip?s.ip+' \u00b7 ':'')+s.wifi;
$('ovUsb').textContent=s.usbWedged?'Wedged':(attached?s.usbStatus:'Waiting\u2026');
$('ovUsb').className='big'+(s.usbWedged?' warn':(attached?'':' off'));
$('ovUsbSub').textContent=s.rawConnected?'RAW client connected':'Listening on port 9100';
$('ovJobs').textContent=s.jobsOk+' done';
$('ovJobs').className='big'+(s.jobsOk?'':' off');
$('ovJobsSub').textContent=s.jobsFailed+' failed'+(s.lastError?' \u00b7 '+s.lastError:' \u00b7 no job errors');
dots($('sysIpp'),s.ippActive?'Running':'Idle',s.ippActive,false);
$('sysRaw').innerHTML='<span class="dot '+(s.rawConnected?'on':'off')+'"></span>'+(s.rawConnected?'Connected':'Idle');
$('sysMdns').innerHTML='<span class="dot '+(s.mdnsReady?'on':'off')+'"></span>'+(s.mdnsReady?'printer.local':'Not started');
$('sysState').textContent=s.printer+(s.printerReason?' \u00b7 '+s.printerReason:'');
$('sysHost').textContent=s.hostname||'\u2014';
$('sysIppErr').textContent=s.ippLastError||'none';
$('sysUsbErr').textContent=s.lastError||'none';
const kb=n=>(n/1024).toFixed(0)+' KB';
const heapT=s.heapTotal||0,psT=s.psramTotal||0;
$('sysHeap').textContent=kb(s.freeHeap)+(heapT?' of '+kb(heapT):'');
bar('heapBar','heapBarWrap',heapT?s.freeHeap/heapT:0,heapT);
$('sysPsram').textContent=kb(s.psramFree)+(psT?' of '+kb(psT):'');
bar('psramBar','psramBarWrap',psT?s.psramFree/psT:0,psT);
$('chipIpp').textContent='http://'+(s.ip||'printer-ip')+':631/ipp/print';
$('chipRaw').textContent=(s.ip||'printer-ip')+':9100';
$('netDot').className='dot '+(s.ip?'on':'warn');
$('netLabel').textContent=s.ip?'Online \u00b7 '+s.ip:s.wifi;
$('updated').textContent='Updated '+new Date().toLocaleTimeString();
}catch(e){}}
setInterval(refresh,5000);refresh();
</script></div></body></html>)HTML");
  const size_t built = strlen(out.c_str());
  Log.printf("[UI] dashboard built: %u of %u bytes\n", (unsigned)built, (unsigned)out.capacity);
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
    const unsigned long now = millis();
    const unsigned long timeSinceDisconnect = now - lastDisconnectMs;
    // If WiFi disconnected recently (less than 10s ago), wait longer before retrying
    if (timeSinceDisconnect < 10000UL) {
      lastWifiAttemptMs = now + 10000UL - timeSinceDisconnect; // Adjust retry timer
      Log.printf("[WiFi] Waiting for %lus after disconnect before retrying; next attempt at %lums\n",
                 10000UL - timeSinceDisconnect,
                 lastWifiAttemptMs);
    } else {
      lastWifiAttemptMs = now;
      Log.printf("[WiFi] Retrying saved network %s (status=%d)\n", config.ssid.c_str(), status);
      WiFi.disconnect(false, false);
      WiFi.begin(config.ssid.c_str(), config.password.c_str());
    }
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

static const char *wifiReasonText(uint8_t reason) {
  switch (reason) {
    case 2: return "AUTH_EXPIRE";
    case 15: return "AUTH_EXPIRE (glitch: both wrong password and this produce this code)";
    case 201: return "NO_AP_FOUND (SSID not heard: hidden network missed, 5 GHz-only router, or name typo)";
    case 202: return "AUTH_FAIL (check password)";
    case 203: return "ASSOC_FAIL (AP refused the association; router-side problem or it is busy)";
    case 204: return "HANDSHAKE_TIMEOUT (typical for a wrong password)";
    case 200: return "BEACON_TIMEOUT (weak signal)";
    default: return "OTHER";
  }
}

void setup() {
  Log.begin(115200); delay(500); Log.println(); SystemDiagnostics::logBoot(); Log.println("=== ESP32-S3 HP Print Server ===");
  loadBootCount();
  startWatchdog();
  Log.printf("[MEM] internal-heap-free=%lu psram-total=%u psram-free=%u\n",
                (unsigned long)ESP.getFreeHeap(), (unsigned)ESP.getPsramSize(),
                (unsigned)ESP.getFreePsram());
  Log.println("[MODE] RAW 9100 (JetDirect/AppSocket) + PCL3GUI-only IPP 631");
  const esp_err_t nvsErr = nvs_flash_init();
  if (nvsErr == ESP_ERR_NVS_NO_FREE_PAGES || nvsErr == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    Log.printf("[CFG] NVS partition stale (err=%d); resetting it\n", (int)nvsErr);
    nvs_flash_erase();
    nvs_flash_init();
  } else if (nvsErr != ESP_OK) {
    Log.printf("[CFG] NVS init error: %s\n", esp_err_to_name(nvsErr));
  }
  loadConfig(); 
  bool wifiOk = false;
  if (!config.ssid.isEmpty()) {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    // The core's internal auto-reconnect retries association at full speed and
    // floods the log on ASSOC_FAIL; our serviceWifi() retry loop handles it with
    // a 30 s backoff instead.
    WiFi.setAutoReconnect(false);
    WiFi.setHostname("printer");
    WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {
      Log.printf("[WiFi] STA disconnected: reason=%d (%s)\n", info.wifi_sta_disconnected.reason, wifiReasonText(info.wifi_sta_disconnected.reason));
    }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
    Log.printf("[WiFi] Connecting to %s (DHCP)\n", config.ssid.c_str());
    WiFi.begin(config.ssid.c_str(), config.password.c_str());
    const unsigned long started = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - started < 20000UL) { delay(250); Log.print('.'); }
    Log.println();
    if (WiFi.status() == WL_CONNECTED) { wifiOk = true; Log.printf("[WiFi] Connected: %s\n", WiFi.localIP().toString().c_str()); }
  }
  if (!wifiOk) { startConfigAP(); }
  lastWifiStatus = (int)WiFi.status();
  lastWifiAttemptMs = 0; // Initialize retry timer
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
