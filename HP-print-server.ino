#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include "usb_printer_backend.h"

// ESP32-S3 USB-to-Wi-Fi RAW print server.
// Network side: JetDirect/AppSocket on TCP 9100 only.
// Print data is forwarded byte-for-byte to the fixed USB Printer Class
// printing interface. No IPP, document conversion, spool or print-language
// emulation is performed here.

// The HP printer's known working RAW printing interface is IF=1 ALT=0.
// Interface selection is intentionally not exposed in the normal UI.
static constexpr const char *RAW_HOSTNAME = "printer";
static constexpr uint8_t RAW_PRINT_INTERFACE = 1;
static constexpr uint8_t RAW_PRINT_ALT = 0;
static constexpr const char *AP_SSID = "HP-Print-Server";
static constexpr const char *AP_PASSWORD = "configureme";
static constexpr const char *CONFIG_NS = "hp-print";

WebServer configServer(80);
Preferences preferences;
UsbHostManager usbHost;
UsbPrinterBackend usbPrinterBackend(usbHost);

struct Config {
  String ssid;
  String password;
};
Config config;
static unsigned long lastStatus = 0;

String esc(String s) {
  s.replace("&", "&amp;");
  s.replace("<", "&lt;");
  s.replace(">", "&gt;");
  s.replace("\"", "&quot;");
  s.replace("'", "&#39;");
  return s;
}

String jsonEsc(String s) {
  s.replace("\\", "\\\\");
  s.replace("\"", "\\\"");
  s.replace("\r", "\\r");
  s.replace("\n", "\\n");
  return s;
}

void defaults() {
  config.ssid = "";
  config.password = "";
}

void loadConfig() {
  defaults();
  if (!preferences.begin(CONFIG_NS, true)) {
    Serial.println("[CFG] Preferences read failed; using defaults");
    return;
  }
  config.ssid = preferences.getString("ssid", config.ssid);
  config.password = preferences.getString("pass", config.password);
  preferences.end();
}

bool saveConfig() {
  if (!preferences.begin(CONFIG_NS, false)) return false;
  bool ok = true;
  ok &= preferences.putString("ssid", config.ssid) > 0 || config.ssid.isEmpty();
  ok &= preferences.putString("pass", config.password) > 0 || config.password.isEmpty();
  preferences.end();
  return ok;
}

bool connectWiFi() {
  if (config.ssid.isEmpty()) {
    Serial.println("[WiFi] No saved SSID");
    return false;
  }

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(RAW_HOSTNAME);
  WiFi.begin(config.ssid.c_str(), config.password.c_str());
  Serial.print("[WiFi] Connecting to ");
  Serial.println(config.ssid);

  const unsigned long deadline = millis() + 20000UL;
  while (WiFi.status() != WL_CONNECTED && millis() < deadline) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("[WiFi] Connection failed, status=%d\n", (int)WiFi.status());
    WiFi.disconnect(false, false);
    return false;
  }

  Serial.print("[WiFi] Connected: ");
  Serial.println(WiFi.localIP());
  Serial.print("[WiFi] Hostname: ");
  Serial.println(RAW_HOSTNAME);
  return true;
}

bool startConfigAP() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.setHostname(RAW_HOSTNAME);
  if (!WiFi.softAP(AP_SSID, AP_PASSWORD, 1, false, 4)) {
    Serial.println("[AP] Failed to start configuration AP");
    return false;
  }
  Serial.print("[AP] SSID: ");
  Serial.println(AP_SSID);
  Serial.print("[AP] Configure at http://");
  Serial.println(WiFi.softAPIP());
  return true;
}

void startRawDiscovery() {
  MDNS.end();
  if (!MDNS.begin(RAW_HOSTNAME)) {
    Serial.println("[mDNS] Failed to start printer.local discovery responder");
    return;
  }
  MDNS.setInstanceName("HP Print Server");
  if (MDNS.addService("pdl-datastream", "tcp", 9100)) {
    MDNS.addServiceTxt("pdl-datastream", "tcp", "txtvers", "1");
    MDNS.addServiceTxt("pdl-datastream", "tcp", "note", "RAW 9100");
    Serial.println("[mDNS] printer.local -> RAW 9100 discovery advertised");
  }
}

String usbStateText() {
  switch (usbHost.state()) {
    case UsbHostManager::STOPPED: return "Stopped";
    case UsbHostManager::RUNNING: return "Running";
    case UsbHostManager::ENUMERATING: return "Enumerating";
    case UsbHostManager::DEVICE_ATTACHED: return "Device attached";
    case UsbHostManager::PRINTER_READY: return "Printer ready";
    case UsbHostManager::ERROR: return String("Error: ") + usbHost.lastError();
  }
  return "Unknown";
}

String printerStateText() {
  switch (usbPrinterBackend.state()) {
    case UsbPrinterBackend::OFFLINE: return "Offline";
    case UsbPrinterBackend::IDLE: return "Ready";
    case UsbPrinterBackend::PRINTING: return "Printing";
    case UsbPrinterBackend::ERROR: return String("Error: ") + usbPrinterBackend.statusReason();
  }
  return "Unknown";
}

String usbStatusText() {
  if (!usbHost.device().attached) return "No printer attached";
  if (!usbHost.portStatusValid()) return "Status pending";

  const uint8_t s = usbHost.portStatusValue();
  String result;
  if (usbHost.portStatusError()) result = "Printer reports an error";
  else if (usbHost.portStatusPaperEmpty()) result = "Paper empty / unavailable";
  else if (usbHost.portStatusSelected()) result = "Printer selected / ready";
  else result = "Printer online";
  result += String(" (0x") + String(s, HEX) + ")";
  return result;
}

String wifiStatusText() {
  if (WiFi.status() == WL_CONNECTED) {
    return String("Connected · ") + WiFi.localIP().toString();
  }
  if (WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA) {
    return String("Setup AP · ") + WiFi.softAPIP().toString();
  }
  return "Not connected";
}

void sendJsonScan() {
  const int n = WiFi.scanNetworks(false, true);
  String out = "[";
  bool first = true;

  for (int i = 0; i < n; ++i) {
    String ssid = WiFi.SSID(i);
    if (ssid.isEmpty()) continue;
    if (!first) out += ",";
    first = false;
    out += String("{\"ssid\":\"") + jsonEsc(ssid)
        + "\",\"rssi\":" + String(WiFi.RSSI(i))
        + ",\"channel\":" + String(WiFi.channel(i)) + "}";
  }

  out += "]";
  WiFi.scanDelete();
  configServer.send(200, "application/json", out);
}

void sendJsonStatus() {
  const bool connected = WiFi.status() == WL_CONNECTED;
  const bool rawActive = usbPrinterBackend.rawClientConnected();
  String out;
  out.reserve(700);
  out += "{\"printer\":\"" + jsonEsc(printerStateText());
  out += "\",\"usb\":\"" + jsonEsc(usbStateText());
  out += "\",\"usbStatus\":\"" + jsonEsc(usbStatusText());
  out += "\",\"wifi\":\"" + jsonEsc(wifiStatusText());
  out += "\",\"rawConnected\":" + String(rawActive ? "true" : "false");
  out += ",\"ip\":\"" + (connected ? WiFi.localIP().toString() : WiFi.softAPIP().toString()) + "\"}";
  configServer.send(200, "application/json", out);
}

String dashboard() {
  const bool connected = WiFi.status() == WL_CONNECTED;
  const String ip = connected ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  const bool printerAttached = usbHost.device().attached;
  const String deviceName = printerAttached && usbHost.device().product.length()
      ? usbHost.device().product
      : "HP USB printer";

  String html;
  html.reserve(15000);

  html += R"HTML(<!doctype html><html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="theme-color" content="#6750A4"><title>HP Print Server</title>
<style>
:root{color-scheme:light;--primary:#6750a4;--primary2:#7f67be;--bg:#f7f5fb;--surface:#fff;--surface2:#f1edf7;--text:#1c1b1f;--muted:#6f6b76;--outline:#ded8e5;--ok:#2e7d32;--warn:#a15c00;--bad:#ba1a1a}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font-family:system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif}
.app{max-width:1050px;margin:auto;padding:24px 18px 52px}.top{display:flex;justify-content:space-between;align-items:center;gap:18px;margin-bottom:22px}.brand{display:flex;align-items:center;gap:14px}.logo{width:46px;height:46px;border-radius:15px;background:var(--primary);color:white;display:grid;place-items:center;font-weight:800;font-size:20px;box-shadow:0 6px 18px #6750a430}.eyebrow{font-size:12px;text-transform:uppercase;letter-spacing:.08em;color:var(--muted);font-weight:700}.title{font-size:28px;font-weight:800;letter-spacing:-.02em}.subtitle{font-size:14px;color:var(--muted);margin-top:2px}.btn{border:0;border-radius:999px;padding:11px 17px;background:var(--primary);color:#fff;font-weight:700;cursor:pointer}.btn.secondary{background:var(--surface2);color:var(--text)}.btn:disabled{opacity:.55;cursor:wait}
.grid{display:grid;grid-template-columns:repeat(4,1fr);gap:12px}.card{background:var(--surface);border:1px solid var(--outline);border-radius:20px;padding:17px;box-shadow:0 4px 20px #1c1b1f08}.label{font-size:12px;color:var(--muted);font-weight:700;text-transform:uppercase;letter-spacing:.05em}.value{font-size:18px;font-weight:800;margin-top:7px;line-height:1.25}.small{font-size:13px;color:var(--muted);margin-top:5px}.pill{display:inline-flex;align-items:center;gap:7px;font-size:12px;font-weight:700;margin-top:9px}.dot{width:9px;height:9px;border-radius:50%;background:var(--ok)}.dot.off{background:var(--bad)}.dot.warn{background:var(--warn)}
.section{margin-top:15px;background:var(--surface);border:1px solid var(--outline);border-radius:22px;padding:20px;box-shadow:0 4px 20px #1c1b1f08}.sectionHead{display:flex;justify-content:space-between;align-items:flex-start;gap:15px;margin-bottom:15px}.section h2{font-size:19px;margin:0}.section p{margin:7px 0;color:var(--muted);font-size:14px;line-height:1.5}.infoGrid{display:grid;grid-template-columns:repeat(3,1fr);gap:10px}.info{background:var(--surface2);border-radius:15px;padding:14px}.info b{display:block;margin-top:4px}.ssidList{display:grid;grid-template-columns:repeat(2,1fr);gap:9px;max-height:360px;overflow:auto;padding:2px}.ssid{border:1px solid var(--outline);background:#fff;border-radius:16px;padding:13px;display:flex;justify-content:space-between;align-items:center;gap:12px;cursor:pointer;text-align:left}.ssid:hover,.ssid.selected{border-color:var(--primary);background:#f8f5ff}.ssidName{font-weight:750;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}.ssidMeta{font-size:12px;color:var(--muted);margin-top:3px}.signal{font-size:12px;font-weight:750;color:var(--muted);white-space:nowrap}.wifiForm{margin-top:16px;border-top:1px solid var(--outline);padding-top:16px}.field{margin-bottom:12px}.field label{display:block;font-size:13px;font-weight:700;margin-bottom:6px}.field input{width:100%;padding:12px 13px;border:1px solid #c9c3d0;border-radius:13px;font:inherit;background:#fff}.hint{font-size:12px;color:var(--muted)}.actions{display:flex;gap:9px;flex-wrap:wrap;align-items:center}.usbBox{display:flex;align-items:center;justify-content:space-between;gap:15px;padding:15px;background:var(--surface2);border-radius:17px}.usbTitle{font-weight:800}.usbDetails{font-size:13px;color:var(--muted);margin-top:4px}.code{font-family:ui-monospace,SFMono-Regular,Consolas,monospace;background:#e9e3f0;padding:3px 6px;border-radius:7px}.toast{position:fixed;left:50%;bottom:24px;transform:translate(-50%,20px);background:#2b2930;color:#fff;padding:12px 16px;border-radius:999px;opacity:0;pointer-events:none;transition:.2s;box-shadow:0 8px 25px #0003;font-size:13px}.toast.show{opacity:1;transform:translate(-50%,0)}
@media(max-width:850px){.grid{grid-template-columns:repeat(2,1fr)}.infoGrid{grid-template-columns:1fr 1fr}.ssidList{grid-template-columns:1fr}}@media(max-width:560px){.app{padding:17px 12px 40px}.top{align-items:flex-start}.title{font-size:23px}.top>.btn{padding:9px 12px}.grid{grid-template-columns:1fr 1fr}.infoGrid{grid-template-columns:1fr}.card{padding:14px}.section{padding:16px;border-radius:18px}.ssidList{max-height:310px}}
</style></head><body><main class="app">
<div class="top"><div class="brand"><div class="logo">HP</div><div><div class="eyebrow">ESP32-S3 print server</div><div class="title">HP Print Server</div><div class="subtitle">RAW JetDirect / AppSocket · no IPP</div></div></div><button class="btn secondary" id="refreshBtn" onclick="refreshStatus()">Refresh</button></div>
<div class="grid">
<div class="card"><div class="label">Printer</div><div id="printer" class="value">)HTML";
  html += esc(printerStateText());
  html += R"HTML(</div><div id="printerPill" class="pill"><span class="dot"></span><span>)HTML";
  html += printerAttached ? "USB connected" : "USB not detected";
  html += R"HTML(</span></div></div>
<div class="card"><div class="label">Wi-Fi</div><div id="wifi" class="value">)HTML";
  html += esc(wifiStatusText());
  html += R"HTML(</div><div class="small">Network connection</div></div>
<div class="card"><div class="label">USB</div><div id="usb" class="value">)HTML";
  html += esc(usbStateText());
  html += R"HTML(</div><div class="small">Fixed print interface 1</div></div>
<div class="card"><div class="label">RAW 9100</div><div id="raw" class="value">Listening</div><div class="small">TCP 9100 · transparent stream</div></div>
</div>

<section class="section"><div class="sectionHead"><div><h2>Printer status</h2><p>Live status comes from the USB printer status path. The printing interface remains fixed and is not exposed as a user setting.</p></div></div>
<div class="infoGrid"><div class="info"><span class="label">Device</span><b>)HTML";
  html += esc(deviceName);
  html += R"HTML(</b></div><div class="info"><span class="label">USB status</span><b id="usbStatus">)HTML";
  html += esc(usbStatusText());
  html += R"HTML(</b></div><div class="info"><span class="label">Connection</span><b id="connection">)HTML";
  html += printerAttached ? "USB attached" : "Waiting for USB printer";
  html += R"HTML(</b></div></div></section>

<section class="section"><div class="sectionHead"><div><h2>Wi-Fi network</h2><p>Select a nearby network. Signal strength is shown for each visible SSID.</p></div><button class="btn secondary" id="scanBtn" onclick="scanWifi()">Scan networks</button></div>
<div id="scanState" class="hint">Press Scan networks to find nearby Wi-Fi.</div><div id="ssidList" class="ssidList" style="margin-top:10px"><div class="info">No scan performed yet.</div></div>
<form class="wifiForm" method="POST" action="/save"><div class="field"><label for="ssid">Selected SSID</label><input id="ssid" name="ssid" value=")HTML";
  html += esc(config.ssid);
  html += R"HTML(" maxlength="32" autocomplete="off" placeholder="Choose a network above or enter a hidden SSID"></div><div class="field"><label for="password">Password</label><input id="password" type="password" name="password" placeholder="Leave blank to keep the saved password"></div><div class="actions"><button class="btn" type="submit">Save Wi-Fi &amp; restart</button><span class="hint">Hidden networks can be entered manually.</span></div></form></section>

<section class="section"><div class="sectionHead"><div><h2>USB connection</h2><p>The known working printer interface is locked to interface 1, alternate setting 0. Endpoint details are intentionally hidden from the normal dashboard.</p></div></div>
<div class="usbBox"><div><div class="usbTitle">RAW printing interface · IF 1 / ALT 0</div><div class="usbDetails">The device is automatically enumerated and the printer status interface is handled by the USB host.</div></div><button class="btn secondary" onclick="scanUsb()">Change / scan USB port</button></div>
<p class="hint" style="margin-top:12px">The scan action is reserved for future USB-port selection. It does not change the active printing interface today.</p></section>

<section class="section"><div class="sectionHead"><div><h2>Connection</h2><p>Use the local hostname when mDNS is available.</p></div></div><div class="infoGrid"><div class="info"><span class="label">Web UI</span><b>printer.local</b></div><div class="info"><span class="label">RAW endpoint</span><b><span class="code">printer.local:9100</span></b></div><div class="info"><span class="label">IP address</span><b id="ip">)HTML";
  html += esc(ip);
  html += R"HTML(</b></div></div></section>

<section class="section"><h2>RAW printing</h2><p>TCP 9100 is transparent: no IPP, Content-Length, PJL, form-feed, document conversion, or other print data is added. The incoming stream is forwarded unchanged to USB.</p><p>Use a print stream understood by the HP printer itself.</p></section>
</main><div id="toast" class="toast"></div>
<script>
const $=id=>document.getElementById(id);
function safe(s){return String(s).replace(/[&<>\"']/g,m=>({'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;',"'":'&#39;'}[m]));}
function signalText(r){if(r>=-55)return 'Excellent';if(r>=-67)return 'Good';if(r>=-75)return 'Fair';return 'Weak';}
async function scanWifi(){const b=$('scanBtn'),list=$('ssidList'),state=$('scanState');b.disabled=true;b.textContent='Scanning…';state.textContent='Scanning nearby networks…';list.innerHTML='<div class="info">Looking for visible networks…</div>';try{const r=await fetch('/scan.json?ts='+Date.now());if(!r.ok)throw new Error();const a=await r.json();a.sort((x,y)=>y.rssi-x.rssi);if(!a.length){list.innerHTML='<div class="info">No visible networks found.</div>';state.textContent='No visible networks found.';return;}list.innerHTML='';a.forEach(x=>{const el=document.createElement('button');el.type='button';el.className='ssid';el.innerHTML='<div style="min-width:0"><div class="ssidName">'+safe(x.ssid)+'</div><div class="ssidMeta">Channel '+x.channel+' · '+signalText(x.rssi)+'</div></div><div class="signal">'+x.rssi+' dBm</div>';el.onclick=()=>{ $('ssid').value=x.ssid;document.querySelectorAll('.ssid').forEach(z=>z.classList.remove('selected'));el.classList.add('selected');};list.appendChild(el);});state.textContent=a.length+' nearby network'+(a.length===1?'':'s')+' found.';}catch(e){list.innerHTML='<div class="info">Wi-Fi scan failed. You can still enter a hidden SSID manually.</div>';state.textContent='Scan failed.';}finally{b.disabled=false;b.textContent='Scan networks';}}
async function refreshStatus(){const b=$('refreshBtn');b.disabled=true;b.textContent='Refreshing…';try{const r=await fetch('/status.json?ts='+Date.now());const s=await r.json();$('printer').textContent=s.printer;$('wifi').textContent=s.wifi;$('usb').textContent=s.usb;$('usbStatus').textContent=s.usbStatus;$('raw').textContent=s.rawConnected?'Job active':'Listening';$('ip').textContent=s.ip;$('connection').textContent=s.usbAttached?'USB attached':'Waiting for USB printer';}catch(e){showToast('Status refresh failed');}finally{b.disabled=false;b.textContent='Refresh';}}
async function scanUsb(){try{const r=await fetch('/usb-scan?ts='+Date.now());const s=await r.json();showToast(s.message);}catch(e){showToast('USB scan request failed');}}
function showToast(t){const x=$('toast');x.textContent=t;x.classList.add('show');clearTimeout(window._toast);window._toast=setTimeout(()=>x.classList.remove('show'),2800);}
setInterval(refreshStatus,5000);
</script></body></html>)HTML";
  return html;
}

void handleRoot() {
  configServer.send(200, "text/html; charset=utf-8", dashboard());
}

void handleSave() {
  if (configServer.hasArg("ssid")) config.ssid = configServer.arg("ssid");
  if (configServer.hasArg("password") && !configServer.arg("password").isEmpty()) {
    config.password = configServer.arg("password");
  }
  saveConfig();
  configServer.send(200, "text/html; charset=utf-8", "<p>Saved. Rebooting…</p>");
  delay(300);
  ESP.restart();
}

void handleUsbScan() {
  // Keep the currently verified printer path. This endpoint is deliberately
  // non-destructive and provides the future hook for USB-port discovery.
  usbHost.setInterfaceSelection(false, RAW_PRINT_INTERFACE, RAW_PRINT_ALT);
  String out = "{\"message\":\"USB scan hook ready. RAW printing remains on interface 1 / alternate 0.\"}";
  configServer.send(200, "application/json", out);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("=== ESP32-S3 RAW 9100 USB Print Server ===");
  Serial.println("[MODE] JetDirect/AppSocket only; IPP disabled");
  Serial.printf("[USB] RAW printing fixed to IF=%u ALT=%u\n", RAW_PRINT_INTERFACE, RAW_PRINT_ALT);

  loadConfig();
  if (!connectWiFi()) startConfigAP();

  startRawDiscovery();

  // The printer's known-good RAW printing interface is fixed here.
  // USB status selection remains descriptor-derived inside UsbHostManager.
  usbHost.setInterfaceSelection(false, RAW_PRINT_INTERFACE, RAW_PRINT_ALT);
  usbPrinterBackend.begin();

  configServer.on("/", HTTP_GET, handleRoot);
  configServer.on("/scan.json", HTTP_GET, sendJsonScan);
  configServer.on("/status.json", HTTP_GET, sendJsonStatus);
  configServer.on("/save", HTTP_POST, handleSave);
  configServer.on("/usb-scan", HTTP_GET, handleUsbScan);
  configServer.begin();

  Serial.println("[HTTP] Configuration server ready");
  Serial.print("[HTTP] Open http://");
  Serial.println(WiFi.status() == WL_CONNECTED ? String(RAW_HOSTNAME) + ".local" : WiFi.softAPIP().toString());
  Serial.println("[RAW] TCP 9100 server enabled");
}

void loop() {
  configServer.handleClient();
  usbHost.poll();
  usbPrinterBackend.poll();

  if (millis() - lastStatus > 5000) {
    lastStatus = millis();
    Serial.printf("[STATUS] WiFi=%d IP=%s USB=%d printer=%s raw=%s usbport=0x%02X\n",
                  (int)WiFi.status(),
                  WiFi.localIP().toString().c_str(),
                  (int)usbHost.state(),
                  printerStateText().c_str(),
                  usbPrinterBackend.rawClientConnected() ? "connected" : "idle",
                  usbHost.portStatusValid() ? usbHost.portStatusValue() : 0);
  }
}
