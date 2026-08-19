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

static constexpr const char *RAW_HOSTNAME = "printer";
static constexpr uint8_t RAW_PRINT_INTERFACE = 1;
static constexpr uint8_t RAW_PRINT_ALT = 0;
static constexpr uint8_t USB_SCAN_INTERFACE = 0;
static constexpr uint8_t USB_SCAN_ALT = 0;
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
static bool scannerInterfaceSelected = false;

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
  const bool printerAttached = usbHost.device().attached;
  String out;
  out.reserve(820);
  out += "{\"printer\":\"" + jsonEsc(printerStateText());
  out += "\",\"usb\":\"" + jsonEsc(usbStateText());
  out += "\",\"usbStatus\":\"" + jsonEsc(usbStatusText());
  out += "\",\"wifi\":\"" + jsonEsc(wifiStatusText());
  out += "\",\"rawConnected\":" + String(rawActive ? "true" : "false");
  out += ",\"usbAttached\":" + String(printerAttached ? "true" : "false");
  out += ",\"interfaceMode\":\"" + String(scannerInterfaceSelected ? "scanner" : "printer") + "\"";
  out += ",\"ip\":\"" + (connected ? WiFi.localIP().toString() : WiFi.softAPIP().toString()) + "\"}";
  configServer.send(200, "application/json", out);
}

void handleInterfaceMode() {
  const String mode = configServer.hasArg("mode") ? configServer.arg("mode") : "printer";
  if (mode == "scanner") {
    scannerInterfaceSelected = true;
    usbHost.setInterfaceSelection(false, USB_SCAN_INTERFACE, USB_SCAN_ALT);
    Serial.printf("[USB] Interface mode changed: scanner IF=%u ALT=%u\n", USB_SCAN_INTERFACE, USB_SCAN_ALT);
    configServer.send(200, "application/json", "{\"mode\":\"scanner\",\"interface\":0,\"alternate\":0}");
    return;
  }

  scannerInterfaceSelected = false;
  usbHost.setInterfaceSelection(false, RAW_PRINT_INTERFACE, RAW_PRINT_ALT);
  Serial.printf("[USB] Interface mode changed: printer IF=%u ALT=%u\n", RAW_PRINT_INTERFACE, RAW_PRINT_ALT);
  configServer.send(200, "application/json", "{\"mode\":\"printer\",\"interface\":1,\"alternate\":0}");
}

String dashboard() {
  const bool connected = WiFi.status() == WL_CONNECTED;
  const String ip = connected ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  const bool printerAttached = usbHost.device().attached;
  const String deviceName = printerAttached && usbHost.device().product.length()
      ? usbHost.device().product
      : "HP USB printer";
  String printerInterfaceDetails = "Not detected";
  if (printerAttached && usbHost.device().printer.found) {
    const UsbPrinterInterfaceInfo &pi = usbHost.device().printer;
    printerInterfaceDetails = String("IF ") + String(pi.interfaceNumber)
        + " · ALT " + String(pi.alternateSetting)
        + " · SUBCLASS 0x" + String(pi.subclass, HEX)
        + " · PROTOCOL 0x" + String(pi.protocol, HEX)
        + " · OUT 0x" + String(pi.bulkOut.address, HEX)
        + " · IN 0x" + String(pi.bulkIn.address, HEX);
  }
  const String scannerInterfaceDetails = String("IF ") + String(USB_SCAN_INTERFACE)
      + " · ALT " + String(USB_SCAN_ALT)
      + " · fixed scanner selection";

  String html;
  html.reserve(17000);

  html += R"HTML(<!doctype html><html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="theme-color" content="#eef2f7"><title>HP Print Server</title>
<style>
:root{color-scheme:light;--bg:#eef2f7;--glass:rgba(255,255,255,.62);--glass-strong:rgba(255,255,255,.78);--line:rgba(255,255,255,.78);--text:#101114;--muted:#69707d;--blue:#007aff;--green:#34c759;--red:#ff3b30;--shadow:0 18px 50px rgba(30,45,70,.12)}
*{box-sizing:border-box}html{background:var(--bg)}body{margin:0;min-height:100vh;color:var(--text);font-family:-apple-system,BlinkMacSystemFont,"SF Pro Display","SF Pro Text","Segoe UI",system-ui,sans-serif;-webkit-font-smoothing:antialiased;background:radial-gradient(circle at 15% 5%,rgba(255,255,255,.95),transparent 35%),radial-gradient(circle at 90% 10%,rgba(190,215,255,.65),transparent 32%),linear-gradient(145deg,#eef2f7,#e7edf5 55%,#f4f6f9);background-attachment:fixed}
body:before{content:"";position:fixed;inset:0;pointer-events:none;background:linear-gradient(120deg,rgba(255,255,255,.24),transparent 35%,rgba(255,255,255,.18));mix-blend-mode:screen}.app{max-width:1050px;margin:auto;padding:24px 18px 52px;position:relative}.top{display:flex;justify-content:space-between;align-items:center;gap:18px;margin-bottom:18px}.brand{display:flex;align-items:center;gap:13px}.logo{width:48px;height:48px;border-radius:16px;background:linear-gradient(145deg,rgba(255,255,255,.86),rgba(215,224,238,.7));border:1px solid rgba(255,255,255,.9);display:grid;place-items:center;font-weight:800;font-size:18px;box-shadow:inset 0 1px 0 #fff,0 10px 28px rgba(30,50,80,.12);backdrop-filter:blur(20px);-webkit-backdrop-filter:blur(20px)}.eyebrow{font-size:11px;text-transform:uppercase;letter-spacing:.08em;color:var(--muted);font-weight:700}.title{font-size:28px;font-weight:800;letter-spacing:-.035em}.subtitle{font-size:13px;color:var(--muted);margin-top:2px}.glass{background:var(--glass);border:1px solid var(--line);box-shadow:var(--shadow),inset 0 1px 0 rgba(255,255,255,.9);backdrop-filter:blur(24px) saturate(145%);-webkit-backdrop-filter:blur(24px) saturate(145%)}.btn{border:1px solid rgba(255,255,255,.35);border-radius:999px;padding:11px 17px;background:var(--blue);color:#fff;font:inherit;font-weight:700;cursor:pointer;box-shadow:0 7px 20px rgba(0,122,255,.2);transition:transform .16s,opacity .16s}.btn:hover{transform:translateY(-1px)}.btn:active{transform:scale(.98)}.btn.secondary{background:rgba(255,255,255,.58);color:var(--text);border-color:rgba(255,255,255,.8);box-shadow:none}.btn:disabled{opacity:.55;cursor:wait;transform:none}
.mode{display:flex;padding:4px;border-radius:16px;margin-bottom:15px;background:rgba(255,255,255,.48);border:1px solid rgba(255,255,255,.78);box-shadow:inset 0 1px 2px rgba(30,50,80,.08)}.mode button{flex:1;border:0;border-radius:12px;padding:10px 15px;background:transparent;color:var(--muted);font:inherit;font-weight:700;cursor:pointer;transition:.18s}.mode button.active{background:var(--glass-strong);color:var(--text);box-shadow:0 4px 14px rgba(30,50,80,.11),inset 0 1px 0 #fff}.modeIcon{margin-right:6px}.interfaceList{display:grid;gap:10px}.interfaceCard{display:block;width:100%;text-align:left;border:1px solid rgba(255,255,255,.7);border-radius:19px;padding:16px;background:rgba(255,255,255,.34);color:var(--text);font:inherit;cursor:pointer;transition:transform .16s,border-color .16s,box-shadow .16s,background .16s}.interfaceCard:hover{transform:translateY(-1px);background:rgba(255,255,255,.5)}.interfaceCard.active{background:rgba(255,255,255,.76);border-color:rgba(0,122,255,.48);box-shadow:0 8px 24px rgba(0,122,255,.12),inset 0 1px 0 #fff}.interfaceTop{display:flex;justify-content:space-between;align-items:center;gap:12px}.interfaceName{font-size:16px;font-weight:800;letter-spacing:-.015em}.interfaceBadge{font-size:11px;font-weight:800;color:var(--muted);padding:5px 9px;border-radius:999px;background:rgba(255,255,255,.6);border:1px solid rgba(255,255,255,.8)}.interfaceCard.active .interfaceBadge{color:var(--blue);background:rgba(0,122,255,.09);border-color:rgba(0,122,255,.15)}.interfaceDetails{margin-top:8px;font-size:12px;line-height:1.6;color:var(--muted);font-family:ui-monospace,SFMono-Regular,Consolas,monospace;white-space:normal}.interfaceDescription{margin-top:5px;font-size:12px;color:var(--muted);font-family:inherit}.grid{display:grid;grid-template-columns:repeat(3,1fr);gap:12px}.card{border-radius:22px;padding:17px;min-height:112px}.label{font-size:11px;color:var(--muted);font-weight:750;text-transform:uppercase;letter-spacing:.06em}.value{font-size:19px;font-weight:800;margin-top:7px;line-height:1.25;letter-spacing:-.02em}.small{font-size:12px;color:var(--muted);margin-top:5px}.pill{display:inline-flex;align-items:center;gap:7px;font-size:12px;font-weight:700;margin-top:9px}.dot{width:8px;height:8px;border-radius:50%;background:var(--green);box-shadow:0 0 0 3px rgba(52,199,89,.12)}.dot.off{background:var(--red);box-shadow:0 0 0 3px rgba(255,59,48,.12)}.section{margin-top:14px;border-radius:24px;padding:20px}.sectionHead{display:flex;justify-content:space-between;align-items:flex-start;gap:15px;margin-bottom:15px}.section h2{font-size:19px;letter-spacing:-.02em;margin:0}.section p{margin:7px 0;color:var(--muted);font-size:13px;line-height:1.5}.infoGrid{display:grid;grid-template-columns:repeat(3,1fr);gap:10px}.info{background:rgba(255,255,255,.38);border:1px solid rgba(255,255,255,.58);border-radius:17px;padding:14px}.info b{display:block;margin-top:5px}.selectWrap{position:relative}.selectWrap:after{content:"⌄";position:absolute;right:15px;top:50%;transform:translateY(-55%);color:var(--muted);pointer-events:none;font-size:18px}.selectWrap select{appearance:none;-webkit-appearance:none;width:100%;padding:14px 42px 14px 14px;border:1px solid rgba(255,255,255,.85);border-radius:15px;background:rgba(255,255,255,.62);color:var(--text);font:inherit;font-weight:650;outline:none;box-shadow:inset 0 1px 0 #fff}.selectWrap select:focus,.field input:focus{border-color:rgba(0,122,255,.55);box-shadow:0 0 0 4px rgba(0,122,255,.1)}.wifiForm{margin-top:16px;border-top:1px solid rgba(255,255,255,.7);padding-top:16px}.field{margin-bottom:12px}.field label{display:block;font-size:12px;font-weight:700;margin-bottom:6px}.field input{width:100%;padding:13px;border:1px solid rgba(255,255,255,.85);border-radius:14px;font:inherit;background:rgba(255,255,255,.58);outline:none}.hint{font-size:12px;color:var(--muted)}.actions{display:flex;gap:9px;flex-wrap:wrap;align-items:center}.printerHero{display:flex;align-items:center;gap:14px}.printerIcon{width:48px;height:48px;border-radius:15px;background:rgba(255,255,255,.55);display:grid;place-items:center;font-size:22px;border:1px solid rgba(255,255,255,.72)}.statusLine{font-size:13px;color:var(--muted);margin-top:4px}.code{font-family:ui-monospace,SFMono-Regular,Consolas,monospace;background:rgba(255,255,255,.5);padding:3px 6px;border-radius:7px}.toast{position:fixed;left:50%;bottom:24px;transform:translate(-50%,20px);background:rgba(25,27,32,.9);color:#fff;padding:12px 16px;border-radius:999px;opacity:0;pointer-events:none;transition:.2s;box-shadow:0 8px 25px rgba(0,0,0,.2);font-size:13px;backdrop-filter:blur(15px);-webkit-backdrop-filter:blur(15px)}.toast.show{opacity:1;transform:translate(-50%,0)}
@media(max-width:760px){.grid{grid-template-columns:1fr 1fr}.infoGrid{grid-template-columns:1fr 1fr}}@media(max-width:560px){.app{padding:16px 12px 40px}.top{align-items:flex-start}.title{font-size:23px}.top>.btn{padding:9px 13px}.grid{grid-template-columns:1fr 1fr}.infoGrid{grid-template-columns:1fr}.section{padding:16px;border-radius:20px}.mode button{padding:9px 10px}.logo{width:44px;height:44px}}
@media(prefers-reduced-motion:reduce){*{scroll-behavior:auto!important;transition:none!important}}
</style></head><body><main class="app">
<div class="top"><div class="brand"><div class="logo">HP</div><div><div class="eyebrow">ESP32-S3 print server</div><div class="title">HP Print Server</div><div class="subtitle">JetDirect / AppSocket · TCP 9100</div></div></div><button class="btn secondary" id="refreshBtn" onclick="refreshStatus()">Refresh</button></div>

<div class="sectionHead" style="margin-top:18px;margin-bottom:10px"><div><h2>Printer Status</h2></div></div>
<div class="grid">
<div class="card glass"><div class="printerHero"><div class="printerIcon">▣</div><div><div class="label">Printer</div><div id="printer" class="value">)HTML";
  html += esc(printerStateText());
  html += R"HTML(</div><div id="printerPill" class="pill"><span class="dot"></span><span>)HTML";
  html += printerAttached ? "USB connected" : "USB not detected";
  html += R"HTML(</span></div></div></div></div>
<div class="card glass"><div class="label">Wi-Fi</div><div id="wifi" class="value">)HTML";
  html += esc(wifiStatusText());
  html += R"HTML(</div><div class="small">Network connection</div></div>
<div class="card glass"><div class="label">RAW 9100</div><div id="raw" class="value">Listening</div><div class="small">Transparent print stream</div></div>
</div>

html += R"HTML(<div class="section glass"><div class="sectionHead"><div><h2>Connection</h2><p>Select which USB interface the server should use. The selected interface is highlighted.</p></div></div>
<div class="interfaceList">
<button type="button" id="printMode" class="interfaceCard active" onclick="setInterfaceMode('printer')"><div class="interfaceTop"><span class="interfaceName">Printer</span><span class="interfaceBadge">Selected</span></div><div class="interfaceDescription">USB Printer Class / RAW printing interface</div><div class="interfaceDetails">)HTML";
  html += esc(printerInterfaceDetails);
  html += R"HTML(</div></button>
<button type="button" id="scanMode" class="interfaceCard" onclick="setInterfaceMode('scanner')"><div class="interfaceTop"><span class="interfaceName">Scanner</span><span class="interfaceBadge">Available</span></div><div class="interfaceDescription">USB scanner interface selection</div><div class="interfaceDetails">)HTML";
  html += esc(scannerInterfaceDetails);
  html += R"HTML(</div></button>
</div></div>

<div class="section glass"><div class="sectionHead"><div><h2>Wi-Fi</h2><p>Configure the network connection independently from USB interface selection.</p></div><button class="btn secondary" id="scanBtn" onclick="scanWifi()">Scan Wi-Fi</button></div>
<div class="selectWrap"><select id="ssidSelect"><option value="">Select a Wi-Fi network…</option></select></div>
<form class="wifiForm" method="POST" action="/save"><div class="field"><label for="ssid">Wi-Fi network</label><input id="ssid" name="ssid" value=")HTML";
  html += esc(config.ssid);
  html += R"HTML(" maxlength="32" autocomplete="off" placeholder="Select a network or enter a hidden SSID"></div><div class="field"><label for="password">Password</label><input id="password" type="password" name="password" placeholder="Leave blank to keep the saved password"></div><div class="actions"><button class="btn" type="submit">Save Wi-Fi &amp; restart</button><span class="hint">Hidden networks can be entered manually.</span></div></form></div>

</main><div id="toast" class="toast"></div>
<script>
const $=id=>document.getElementById(id);
let interfaceMode=')HTML";
  html += scannerInterfaceSelected ? "scanner" : "printer";
  html += R"HTML(';
function safe(s){return String(s).replace(/[&<>\"']/g,m=>({'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;',"'":'&#39;'}[m]));}
function setInterfaceButtons(){const scanner=interfaceMode==='scanner';$('scanMode').classList.toggle('active',scanner);$('printMode').classList.toggle('active',!scanner);$('scanMode').querySelector('.interfaceBadge').textContent=scanner?'Selected':'Available';$('printMode').querySelector('.interfaceBadge').textContent=scanner?'Available':'Selected';}
async function setInterfaceMode(mode){if(mode===interfaceMode)return;const old=interfaceMode;interfaceMode=mode;setInterfaceButtons();try{const r=await fetch('/interface.json?mode='+encodeURIComponent(mode)+'&ts='+Date.now());if(!r.ok)throw new Error();const s=await r.json();interfaceMode=s.mode;setInterfaceButtons();showToast(interfaceMode==='scanner'?'Scanner interface selected (IF 0)':'Printer interface selected (IF 1)');refreshStatus();}catch(e){interfaceMode=old;setInterfaceButtons();showToast('USB interface change failed');}}
$('ssidSelect').addEventListener('change',()=>{$('ssid').value=$('ssidSelect').value;});
async function scanWifi(){const b=$('scanBtn'),select=$('ssidSelect');b.disabled=true;b.textContent='Scanning…';select.innerHTML='<option value="">Scanning…</option>';try{const r=await fetch('/scan.json?ts='+Date.now());if(!r.ok)throw new Error();const a=await r.json();a.sort((x,y)=>y.rssi-x.rssi);select.innerHTML='<option value="">Select a Wi-Fi network…</option>';a.forEach(x=>{const o=document.createElement('option');o.value=x.ssid;o.textContent=x.ssid+'  ·  '+x.rssi+' dBm  ·  Ch '+x.channel;select.appendChild(o);});if(!a.length){select.innerHTML='<option value="">No visible networks found</option>'}else{$('scanState').textContent=a.length+' nearby Wi-Fi network'+(a.length===1?'':'s')+' found.';$('scanState').style.display='block';const current=$('ssid').value;if(current&&a.some(x=>x.ssid===current))select.value=current;}}catch(e){select.innerHTML='<option value="">Scan failed</option>'}finally{b.disabled=false;b.textContent='Scan Wi-Fi';}}
async function refreshStatus(){const b=$('refreshBtn');b.disabled=true;b.textContent='Refreshing…';try{const r=await fetch('/status.json?ts='+Date.now());if(!r.ok)throw new Error();const s=await r.json();$('printer').textContent=s.printer;$('wifi').textContent=s.wifi;$('usbStatus').textContent=s.usbStatus;$('raw').textContent=s.rawConnected?'Job active':'Listening';$('ip').textContent=s.ip;$('connection').textContent=s.usbAttached?'USB attached':'Waiting for USB printer';interfaceMode=s.interfaceMode||interfaceMode;setInterfaceButtons();$('printerPill').innerHTML='<span class="dot '+(s.usbAttached?'':'off')+'"></span><span>'+(s.usbAttached?'USB connected':'USB not detected')+'</span>';}catch(e){showToast('Status refresh failed');}finally{b.disabled=false;b.textContent='Refresh';}}
function showToast(t){const x=$('toast');x.textContent=t;x.classList.add('show');clearTimeout(window._toast);window._toast=setTimeout(()=>x.classList.remove('show'),2800);}
setInterfaceButtons();
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
  usbHost.setInterfaceSelection(false, RAW_PRINT_INTERFACE, RAW_PRINT_ALT);
  String out = "{\"message\":\"USB printer interface selected: IF 1 / alternate 0.\"}";
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

  scannerInterfaceSelected = false;
  usbHost.setInterfaceSelection(false, RAW_PRINT_INTERFACE, RAW_PRINT_ALT);
  usbPrinterBackend.begin();

  configServer.on("/", HTTP_GET, handleRoot);
  configServer.on("/scan.json", HTTP_GET, sendJsonScan);
  configServer.on("/status.json", HTTP_GET, sendJsonStatus);
  configServer.on("/interface.json", HTTP_GET, handleInterfaceMode);
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

  if (millis() - lastStatus > 1000) {
    lastStatus = millis();

    const int wifiState = (int)WiFi.status();
    const String ip = WiFi.localIP().toString();
    const int usbState = (int)usbHost.state();
    const String printerState = printerStateText();
    const bool rawConnected = usbPrinterBackend.rawClientConnected();
    const uint8_t usbPort =
        usbHost.portStatusValid() ? usbHost.portStatusValue() : 0;

    static bool initialized = false;
    static int lastWifiState = -1;
    static String lastIp;
    static int lastUsbState = -1;
    static String lastPrinterState;
    static bool lastRawConnected = false;
    static uint8_t lastUsbPort = 0;

    const bool changed =
        !initialized ||
        wifiState != lastWifiState ||
        ip != lastIp ||
        usbState != lastUsbState ||
        printerState != lastPrinterState ||
        rawConnected != lastRawConnected ||
        usbPort != lastUsbPort;

    if (changed) {
      Serial.printf("[STATUS] WiFi=%d IP=%s USB=%d printer=%s raw=%s usbport=0x%02X\n",
                    wifiState,
                    ip.c_str(),
                    usbState,
                    printerState.c_str(),
                    rawConnected ? "connected" : "idle",
                    usbPort);

      lastWifiState = wifiState;
      lastIp = ip;
      lastUsbState = usbState;
      lastPrinterState = printerState;
      lastRawConnected = rawConnected;
      lastUsbPort = usbPort;
      initialized = true;
    }
  }
}
