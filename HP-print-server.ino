#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include "usb_printer_backend.h"
#include "ipp_pcl3_service.h"

// ESP32-S3 USB-to-Wi-Fi print server.
// Network side: RAW JetDirect/AppSocket on TCP 9100 plus IPP on TCP 631.
// IPP advertises PCL3GUI only and forwards the IPP document payload to the
// same classic USB Printer Class interface used by RAW printing. No IPP-over-USB
// interface is selected or used for printing.

static constexpr const char *RAW_HOSTNAME = "printer";
static constexpr const char *AP_SSID = "HP-Print-Server";
static constexpr const char *AP_PASSWORD = "configureme";
static constexpr const char *CONFIG_NS = "hp-print";

WebServer configServer(80);
Preferences preferences;
UsbHostManager usbHost;
UsbPrinterBackend usbPrinterBackend(usbHost);
IppPcl3Service ippService(usbPrinterBackend);

void ensureUsbScannerWebRoutesInstalled();

struct Config {
  String ssid;
  String password;
};

Config config;
static unsigned long lastStatus = 0;
static bool mdnsReady = false;
static bool configApActive = false;

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
  const size_t ssidWritten = preferences.putString("ssid", config.ssid);
  const size_t passWritten = preferences.putString("pass", config.password);
  preferences.end();
  const bool ssidOk = config.ssid.isEmpty() || ssidWritten > 0;
  const bool passOk = config.password.isEmpty() || passWritten > 0;
  return ssidOk && passOk;
}

bool connectWiFi() {
  if (config.ssid.isEmpty()) {
    Serial.println("[WiFi] No saved SSID");
    return false;
  }

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(RAW_HOSTNAME);
  WiFi.begin(config.ssid.c_str(), config.password.c_str());
  Serial.printf("[WiFi] Connecting to %s\n", config.ssid.c_str());

  const unsigned long started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 20000UL) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("[WiFi] Connection failed, status=%d\n", (int)WiFi.status());
    WiFi.disconnect(false, false);
    return false;
  }

  Serial.printf("[WiFi] Connected: %s\n", WiFi.localIP().toString().c_str());
  return true;
}

bool startConfigAP() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.setHostname(RAW_HOSTNAME);
  if (!WiFi.softAP(AP_SSID, AP_PASSWORD, 1, false, 4)) {
    Serial.println("[AP] Failed to start configuration AP");
    configApActive = false;
    return false;
  }
  configApActive = true;
  Serial.printf("[AP] SSID: %s\n", AP_SSID);
  Serial.printf("[AP] Configure at http://%s\n", WiFi.softAPIP().toString().c_str());
  return true;
}

void startRawDiscovery() {
  MDNS.end();
  mdnsReady = false;
  if (!MDNS.begin(RAW_HOSTNAME)) {
    Serial.println("[mDNS] Failed to start printer.local discovery responder");
    return;
  }

  mdnsReady = true;
  MDNS.setInstanceName("HP Print Server");
  if (MDNS.addService("pdl-datastream", "tcp", 9100)) {
    MDNS.addServiceTxt("pdl-datastream", "tcp", "txtvers", "1");
    MDNS.addServiceTxt("pdl-datastream", "tcp", "note", "RAW 9100");
    Serial.println("[mDNS] printer.local -> RAW 9100 discovery advertised");
  }

  if (MDNS.addService("ipp", "tcp", 631)) {
    MDNS.addServiceTxt("ipp", "tcp", "txtvers", "1");
    MDNS.addServiceTxt("ipp", "tcp", "qtotal", "1");
    MDNS.addServiceTxt("ipp", "tcp", "rp", "ipp/print");
    MDNS.addServiceTxt("ipp", "tcp", "ty", "HP Smart Tank 520_540 series");
    MDNS.addServiceTxt("ipp", "tcp", "product", "(HP Smart Tank 520_540 series)");
    MDNS.addServiceTxt("ipp", "tcp", "pdl", "application/vnd.hp-PCL");
    MDNS.addServiceTxt("ipp", "tcp", "usb_MFG", "HP");
    MDNS.addServiceTxt("ipp", "tcp", "usb_MDL", "HP Smart Tank 520_540 series");
    MDNS.addServiceTxt("ipp", "tcp", "usb_CMD", "PCL3GUI");
    Serial.println("[mDNS] printer.local -> IPP 631 advertised as PCL3GUI only");
  }
}

String printerStateText() {
  switch (usbPrinterBackend.state()) {
    case UsbPrinterBackend::OFFLINE: return "Offline";
    case UsbPrinterBackend::IDLE: return "Ready";
    case UsbPrinterBackend::PRINTING: return "Printing";
    case UsbPrinterBackend::ERROR: return "Printer error";
  }
  return "Unknown";
}

String printerReasonText() {
  const String &reason = usbPrinterBackend.statusReason();
  if (reason == "waiting-for-usb-printer") return "Connect a compatible USB printer";
  if (reason == "enumerating-usb-device") return "Checking the USB printer";
  if (reason == "printer-interface-ready" || reason == "usb-printer-ready") return "Printer is ready";
  if (reason == "raw-job-in-progress") return "Sending the print job";
  if (reason == "raw-job-draining") return "Finishing the print job";
  if (reason == "usb-printer-reports-paper-empty") return "Paper is empty or unavailable";
  if (reason == "usb-printer-reports-not-selected") return "Printer is not selected or not ready";
  if (reason == "usb-printer-reports-error" || reason == "usb-printer-reports-error-after-job") return "Printer reported an error";
  if (reason == "selected-interface-has-no-bulk-output") return "No compatible RAW printing interface was found";
  if (reason.startsWith("USB bulk transfer failed") || reason.startsWith("USB Bulk OUT")) return "USB transfer to the printer failed";
  return reason.length() ? reason : "Waiting for printer";
}

String usbStatusText() {
  if (!usbHost.device().attached) return "No USB printer detected";
  if (!usbHost.portStatusValid()) return "Printer connected; detailed status is not available yet";
  if (usbHost.portStatusError()) return "Printer reports an error";
  if (usbHost.portStatusPaperEmpty()) return "Paper is empty or unavailable";
  if (!usbHost.portStatusSelected()) return "Printer is not selected or not ready";
  return "Printer is selected and ready";
}

String wifiStatusText() {
  if (WiFi.status() == WL_CONNECTED) return String("Connected · ") + WiFi.localIP().toString();
  if (configApActive) return String("Setup access point · ") + WiFi.softAPIP().toString();
  return "Not connected";
}

String activeNetworkName() {
  if (WiFi.status() == WL_CONNECTED) return WiFi.SSID();
  if (configApActive) return String(AP_SSID);
  return "Not connected";
}

String activeIp() {
  if (WiFi.status() == WL_CONNECTED) return WiFi.localIP().toString();
  if (configApActive) return WiFi.softAPIP().toString();
  return "";
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
  const bool printerAttached = usbHost.device().attached;
  const String printerName = printerAttached && usbHost.device().product.length()
      ? usbHost.device().product : "USB printer";
  String out;
  out.reserve(1000);
  out += "{\"printer\":\"" + jsonEsc(printerStateText());
  out += "\",\"printerReason\":\"" + jsonEsc(printerReasonText());
  out += "\",\"printerName\":\"" + jsonEsc(printerName);
  out += "\",\"usbStatus\":\"" + jsonEsc(usbStatusText());
  out += "\",\"wifi\":\"" + jsonEsc(wifiStatusText());
  out += "\",\"ssid\":\"" + jsonEsc(activeNetworkName());
  out += "\",\"rawConnected\":" + String(usbPrinterBackend.rawClientConnected() ? "true" : "false");
  out += ",\"usbAttached\":" + String(printerAttached ? "true" : "false");
  out += ",\"ip\":\"" + jsonEsc(activeIp());
  out += "\",\"mdnsReady\":" + String(mdnsReady ? "true" : "false");
  out += ",\"hostname\":\"" + String(mdnsReady ? "printer.local" : "") + "\"}";
  configServer.send(200, "application/json", out);
}

String dashboard() {
  const bool printerAttached = usbHost.device().attached;
  const String deviceName = printerAttached && usbHost.device().product.length()
      ? usbHost.device().product : "No USB printer";
  const String ip = activeIp();
  const String networkName = activeNetworkName();

  String html;
  html.reserve(9000);
  html += R"HTML(<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><meta name="theme-color" content="#f3f5f7"><title>Print Server</title><style>
*{box-sizing:border-box}body{margin:0;background:#f3f5f7;color:#344054;font-family:system-ui,-apple-system,"Segoe UI",Roboto,Arial,sans-serif;font-size:14px;line-height:1.5;-webkit-font-smoothing:antialiased}.app{max-width:940px;margin:auto;padding:22px 16px 40px}.top{display:flex;align-items:center;justify-content:space-between;gap:14px;margin-bottom:16px}.title{font-size:25px;font-weight:650;letter-spacing:-.02em}.subtitle,.small,.hint,.statusLine,.section p{color:#758195;font-size:13px}.actions{display:flex;gap:8px;align-items:center;flex-wrap:wrap}.btn{border:1px solid #d8e1e8;border-radius:9px;padding:8px 12px;background:#edf3f7;color:#466681;font:inherit;font-weight:600;cursor:pointer}.btn.primary{background:#557b9a;color:#fff;border-color:#557b9a}.btn:disabled{opacity:.55}.grid{display:grid;grid-template-columns:repeat(3,1fr);gap:10px}.card,.section{background:#fbfcfd;border:1px solid #e1e7ec;border-radius:14px}.card{padding:15px;min-height:112px}.section{margin-top:11px;padding:17px}.label,.detailKey{font-size:11px;text-transform:uppercase;letter-spacing:.045em;color:#7b8797;font-weight:600}.value{font-size:18px;font-weight:650;margin-top:6px;line-height:1.3}.pill{display:flex;align-items:center;gap:7px;font-size:12px;color:#637083;margin-top:8px}.dot{width:8px;height:8px;border-radius:50%;background:#719b7d}.dot.off{background:#b47b72}.sectionHead{display:flex;justify-content:space-between;align-items:flex-start;gap:12px;margin-bottom:12px}.section h2{font-size:17px;font-weight:650;margin:0}.section p{margin:4px 0}.details{display:grid;grid-template-columns:repeat(2,1fr);gap:8px}.detail,.service{padding:11px 12px;border-radius:10px;background:#f6f8fa;border:1px solid #e7ebef}.detailValue{font-size:13px;font-weight:600;margin-top:3px;word-break:break-word}.address{font-family:ui-monospace,SFMono-Regular,Consolas,monospace;font-size:12px;margin-top:6px;word-break:break-all;color:#536272}.selectWrap select,.field input{width:100%;padding:10px 11px;border:1px solid #dce3e9;border-radius:9px;background:#fff;color:#344054;font:inherit;outline:none}.selectWrap select:focus,.field input:focus{border-color:#9eb5c7}.wifiForm{margin-top:13px;border-top:1px solid #e7ebef;padding-top:13px}.field{margin-bottom:10px}.field label{display:block;font-size:12px;font-weight:600;margin-bottom:5px;color:#596779}.check{display:flex;align-items:center;gap:8px;font-size:12px;color:#758195;margin:0 0 12px}.toast{position:fixed;left:50%;bottom:18px;transform:translate(-50%,15px);background:#475467;color:#fff;padding:9px 13px;border-radius:9px;opacity:0;transition:.18s}.toast.show{opacity:1;transform:translate(-50%,0)}a.btn{text-decoration:none}@media(max-width:720px){.grid{grid-template-columns:1fr}.details{grid-template-columns:1fr}}@media(max-width:520px){.app{padding:16px 11px 32px}.title{font-size:22px}.top{align-items:flex-start}.section{padding:14px}}
</style></head><body><main class="app"><div class="top"><div><div class="title">Print Server</div><div class="subtitle">USB printer over Wi-Fi</div></div><div class="actions"><a class="btn" href="/scan">Scanner (disabled)</a><button class="btn" id="refreshBtn" onclick="refreshStatus()">Refresh</button></div></div><div class="grid"><div class="card"><div class="label">Printer</div><div id="printerName" class="value">)HTML";
  html += esc(deviceName);
  html += R"HTML(</div><div id="printerState" class="small">)HTML";
  html += esc(printerStateText());
  html += R"HTML(</div><div id="printerReason" class="small">)HTML";
  html += esc(printerReasonText());
  html += R"HTML(</div><div id="printerPill" class="pill"><span class="dot )HTML";
  html += printerAttached ? "" : "off";
  html += R"HTML("></span><span>)HTML";
  html += printerAttached ? "USB connected" : "USB not detected";
  html += R"HTML(</span></div></div><div class="card"><div class="label">Network</div><div id="wifiName" class="value">)HTML";
  html += esc(networkName);
  html += R"HTML(</div><div id="wifiState" class="small">)HTML";
  html += esc(wifiStatusText());
  html += R"HTML(</div></div><div class="card"><div class="label">RAW printing</div><div id="raw" class="value">)HTML";
  html += usbPrinterBackend.rawClientConnected() ? "Active" : (usbPrinterBackend.online() ? "Ready" : "Unavailable");
  html += R"HTML(</div><div class="small">JetDirect / AppSocket · TCP 9100</div></div></div><div class="section"><div class="sectionHead"><div><h2>Connection</h2><p>Essential device and network details.</p></div></div><div class="details"><div class="detail"><div class="detailKey">USB</div><div id="usbDetail" class="detailValue">)HTML";
  html += printerAttached ? "Connected" : "Not detected";
  html += R"HTML(</div></div><div class="detail"><div class="detailKey">Printer status</div><div id="usbStatus" class="detailValue">)HTML";
  html += esc(usbStatusText());
  html += R"HTML(</div></div><div class="detail"><div class="detailKey">IP address</div><div id="ipDetail" class="detailValue">)HTML";
  html += esc(ip.length() ? ip : "Unavailable");
  html += R"HTML(</div></div><div class="detail"><div class="detailKey">Hostname</div><div id="hostDetail" class="detailValue">)HTML";
  html += mdnsReady ? "printer.local" : "Unavailable";
  html += R"HTML(</div></div></div></div><div class="section"><div class="sectionHead"><div><h2>Print addresses</h2><p>RAW remains available; IPP advertises only HP PCL3GUI.</p></div></div><div class="service"><div id="printAddress" class="address">)HTML";
  html += mdnsReady ? "socket://printer.local:9100" : (ip.length() ? String("socket://") + ip + ":9100" : "Unavailable");
  html += R"HTML(</div></div><div class="service" style="margin-top:8px"><div class="label">IPP · PCL3GUI only</div><div class="address">ipp://printer.local:631/ipp/print</div>)HTML";
  html += R"HTML(</div></div></div><div class="section"><div class="sectionHead"><div><h2>Wi-Fi</h2><p>Change the network used by this print server.</p></div><button class="btn" id="scanBtn" onclick="scanWifi()">Scan networks</button></div><div class="selectWrap"><select id="ssidSelect"><option value="">Select a Wi-Fi network…</option></select></div><div id="scanState" class="statusLine" style="display:none"></div><form class="wifiForm" method="POST" action="/save"><div class="field"><label for="ssid">Wi-Fi network</label><input id="ssid" name="ssid" value=")HTML";
  html += esc(config.ssid);
  html += R"HTML(" maxlength="32" autocomplete="off" placeholder="Select a network or enter a hidden SSID"></div><div class="field"><label for="password">Password</label><input id="password" type="password" name="password" placeholder="Leave blank to keep the saved password"></div><label class="check"><input type="checkbox" name="clearPassword" value="1"> Clear saved password for an open network</label><div class="actions"><button class="btn primary" type="submit">Save &amp; restart</button><span class="hint">Hidden SSIDs can be entered manually.</span></div></form></div></main><div id="toast" class="toast"></div><script>
const $=id=>document.getElementById(id);$('ssidSelect').addEventListener('change',()=>{$('ssid').value=$('ssidSelect').value;});async function scanWifi(){const b=$('scanBtn'),select=$('ssidSelect');b.disabled=true;b.textContent='Scanning…';select.innerHTML='<option value="">Scanning…</option>';try{const r=await fetch('/scan.json?ts='+Date.now());if(!r.ok)throw new Error();const a=await r.json();a.sort((x,y)=>y.rssi-x.rssi);select.innerHTML='<option value="">Select a Wi-Fi network…</option>';a.forEach(x=>{const o=document.createElement('option');o.value=x.ssid;o.textContent=x.ssid+' · '+x.rssi+' dBm · Ch '+x.channel;select.appendChild(o);});$('scanState').style.display='block';$('scanState').textContent=a.length?a.length+' network'+(a.length===1?'':'s')+' found.':'No visible Wi-Fi networks found.';}catch(e){select.innerHTML='<option value="">Scan failed</option>';$('scanState').style.display='block';$('scanState').textContent='Wi-Fi scan failed.';}finally{b.disabled=false;b.textContent='Scan networks';}}async function refreshStatus(){const b=$('refreshBtn');b.disabled=true;b.textContent='Refreshing…';try{const r=await fetch('/status.json?ts='+Date.now());if(!r.ok)throw new Error();const s=await r.json();$('printerName').textContent=s.usbAttached?s.printerName:'No USB printer';$('printerState').textContent=s.printer;$('printerReason').textContent=s.printerReason;$('wifiName').textContent=s.ssid||'Not connected';$('wifiState').textContent=s.wifi;$('raw').textContent=s.rawConnected?'Active':(s.printer==='Ready'?'Ready':'Unavailable');$('usbDetail').textContent=s.usbAttached?'Connected':'Not detected';$('usbStatus').textContent=s.usbStatus;$('ipDetail').textContent=s.ip||'Unavailable';$('hostDetail').textContent=s.mdnsReady?'printer.local':'Unavailable';$('printAddress').textContent=s.mdnsReady?'socket://printer.local:9100':(s.ip?'socket://'+s.ip+':9100':'Unavailable');$('printerPill').innerHTML='<span class="dot '+(s.usbAttached?'':'off')+'"></span><span>'+(s.usbAttached?'USB connected':'USB not detected')+'</span>';}catch(e){showToast('Status refresh failed');}finally{b.disabled=false;b.textContent='Refresh';}}function showToast(t){const x=$('toast');x.textContent=t;x.classList.add('show');clearTimeout(window._toast);window._toast=setTimeout(()=>x.classList.remove('show'),2400);}setInterval(refreshStatus,5000);
</script></body></html>)HTML";
  return html;
}

void handleRoot() {
  configServer.send(200, "text/html; charset=utf-8", dashboard());
}

void handleSave() {
  if (!configServer.hasArg("ssid") || configServer.arg("ssid").isEmpty()) {
    configServer.send(400, "text/plain; charset=utf-8", "Wi-Fi network name is required.");
    return;
  }

  config.ssid = configServer.arg("ssid");
  if (configServer.hasArg("clearPassword")) {
    config.password = "";
  } else if (configServer.hasArg("password") && !configServer.arg("password").isEmpty()) {
    config.password = configServer.arg("password");
  }

  if (!saveConfig()) {
    configServer.send(500, "text/plain; charset=utf-8", "Could not save Wi-Fi settings. The device was not restarted.");
    return;
  }

  configServer.send(200, "text/html; charset=utf-8", "<p>Wi-Fi settings saved. Rebooting…</p>");
  delay(300);
  ESP.restart();
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("=== ESP32-S3 HP PCL3GUI Print Server ===");
  Serial.println("[MODE] RAW 9100 + IPP 631 PCL3GUI; both print through the classic USB Printer Class interface");
  Serial.println("[MODE] IPP-over-USB printing is disabled/not used; scanner USB backend is disabled");

  loadConfig();
  if (!connectWiFi()) startConfigAP();
  startRawDiscovery();

  usbPrinterBackend.begin();
  ippService.begin();

  configServer.on("/", HTTP_GET, handleRoot);
  configServer.on("/scan.json", HTTP_GET, sendJsonScan);
  configServer.on("/status.json", HTTP_GET, sendJsonStatus);
  configServer.on("/save", HTTP_POST, handleSave);
  ensureUsbScannerWebRoutesInstalled();
  configServer.begin();

  Serial.println("[HTTP] Configuration server ready");
  Serial.printf("[HTTP] Open http://%s\n",
                mdnsReady ? "printer.local" : (activeIp().length() ? activeIp().c_str() : "device-address"));
  Serial.println("[RAW] TCP 9100 server enabled");
  Serial.println("[IPP] TCP 631 service enabled; document-format-supported=application/vnd.hp-PCL; version=PCL3GUI");
}

void loop() {
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
    const bool usbStatusValid = usbHost.portStatusValid();
    const uint8_t usbPort = usbStatusValid ? usbHost.portStatusValue() : 0;

    static bool initialized = false;
    static int lastWifiState = -1;
    static String lastIp;
    static int lastUsbState = -1;
    static String lastPrinterState;
    static bool lastRawConnected = false;
    static bool lastUsbStatusValid = false;
    static uint8_t lastUsbPort = 0;

    const bool changed = !initialized || wifiState != lastWifiState || ip != lastIp ||
        usbState != lastUsbState || printerState != lastPrinterState ||
        rawConnected != lastRawConnected || usbStatusValid != lastUsbStatusValid ||
        (usbStatusValid && usbPort != lastUsbPort);

    if (changed) {
      Serial.printf("[STATUS] WiFi=%d IP=%s USB=%d printer=%s raw=%s%s\n",
                    wifiState, ip.c_str(), usbState, printerState.c_str(),
                    rawConnected ? "connected" : "idle",
                    usbStatusValid ? (String(" usbport=0x") + String(usbPort, HEX)).c_str() : " usbstatus=unavailable");
      lastWifiState = wifiState;
      lastIp = ip;
      lastUsbState = usbState;
      lastPrinterState = printerState;
      lastRawConnected = rawConnected;
      lastUsbStatusValid = usbStatusValid;
      lastUsbPort = usbPort;
      initialized = true;
    }
  }
}
