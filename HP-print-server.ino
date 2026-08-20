#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include "usb_printer_backend.h"

// ESP32-S3 USB-to-Wi-Fi RAW print server.
// Network side: JetDirect/AppSocket on TCP 9100 only.
// Print data is forwarded byte-for-byte to an automatically selected classic
// USB Printer Class interface. IPP-over-USB is intentionally not used here.

static constexpr const char *RAW_HOSTNAME = "printer";
static constexpr const char *AP_SSID = "HP-Print-Server";
static constexpr const char *AP_PASSWORD = "configureme";
static constexpr const char *CONFIG_NS = "hp-print";
static constexpr unsigned long WIFI_RETRY_INTERVAL_MS = 10000;

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
static unsigned long lastWifiRetry = 0;
static bool mdnsReady = false;
static bool configApActive = false;
static bool wifiSleepConfigured = false;

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

void configureConnectedWiFi() {
  if (WiFi.status() != WL_CONNECTED || wifiSleepConfigured) return;
  WiFi.setSleep(false);
  wifiSleepConfigured = true;
  Serial.println("[WiFi] Power saving disabled for mDNS reliability");
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
    wifiSleepConfigured = false;
    return false;
  }

  configureConnectedWiFi();
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
  if (WiFi.status() != WL_CONNECTED) {
    mdnsReady = false;
    Serial.println("[mDNS] Not started: Wi-Fi is not connected");
    return;
  }

  MDNS.end();
  mdnsReady = false;
  if (!MDNS.begin(RAW_HOSTNAME)) {
    Serial.printf("[mDNS] Failed to start %s.local at %s\n",
                  RAW_HOSTNAME, WiFi.localIP().toString().c_str());
    return;
  }

  mdnsReady = true;
  MDNS.setInstanceName("HP Print Server");

  const bool httpService = MDNS.addService("http", "tcp", 80);
  const bool rawService = MDNS.addService("pdl-datastream", "tcp", 9100);
  if (rawService) {
    MDNS.addServiceTxt("pdl-datastream", "tcp", "txtvers", "1");
    MDNS.addServiceTxt("pdl-datastream", "tcp", "note", "RAW 9100");
  }

  Serial.printf("[mDNS] Hostname active: %s.local -> %s\n",
                RAW_HOSTNAME, WiFi.localIP().toString().c_str());
  Serial.printf("[mDNS] Services: HTTP/80=%s RAW/9100=%s\n",
                httpService ? "advertised" : "failed",
                rawService ? "advertised" : "failed");
  if (!httpService || !rawService) {
    Serial.println("[mDNS] Hostname responder is active, but one or more service advertisements failed");
  }
}

void maintainWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    configureConnectedWiFi();
    if (configApActive) {
      WiFi.softAPdisconnect(true);
      configApActive = false;
      Serial.println("[AP] Configuration access point stopped after Wi-Fi recovery");
    }
    if (!mdnsReady) startRawDiscovery();
    return;
  }

  if (mdnsReady) {
    Serial.println("[mDNS] Wi-Fi disconnected; hostname responder will restart after reconnection");
    MDNS.end();
  }
  mdnsReady = false;
  wifiSleepConfigured = false;
  if (config.ssid.isEmpty()) return;
  if (millis() - lastWifiRetry < WIFI_RETRY_INTERVAL_MS) return;
  lastWifiRetry = millis();
  Serial.println("[WiFi] Connection lost; retrying saved network");
  WiFi.mode(configApActive ? WIFI_AP_STA : WIFI_STA);
  WiFi.setHostname(RAW_HOSTNAME);
  WiFi.begin(config.ssid.c_str(), config.password.c_str());
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
  html.reserve(13000);
  html += R"HTML(<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><meta name="theme-color" content="#eef2f7"><title>HP Print Server</title><style>
:root{color-scheme:light;--bg:#eef2f7;--glass:rgba(255,255,255,.66);--line:rgba(255,255,255,.82);--text:#101114;--muted:#69707d;--blue:#007aff;--green:#34c759;--red:#ff3b30;--shadow:0 18px 50px rgba(30,45,70,.12)}*{box-sizing:border-box}body{margin:0;min-height:100vh;color:var(--text);font-family:-apple-system,BlinkMacSystemFont,"SF Pro Display","SF Pro Text","Segoe UI",system-ui,sans-serif;background:radial-gradient(circle at 15% 5%,#fff,transparent 35%),linear-gradient(145deg,#eef2f7,#e7edf5 55%,#f4f6f9);-webkit-font-smoothing:antialiased}.app{max-width:1050px;margin:auto;padding:24px 18px 52px}.top{display:flex;justify-content:space-between;align-items:center;gap:18px;margin-bottom:18px}.brand{display:flex;align-items:center;gap:13px}.logo{width:48px;height:48px;border-radius:16px;background:rgba(255,255,255,.72);border:1px solid #fff;display:grid;place-items:center;font-weight:800;box-shadow:var(--shadow)}.eyebrow,.label,.detailKey{font-size:11px;text-transform:uppercase;letter-spacing:.07em;color:var(--muted);font-weight:750}.title{font-size:28px;font-weight:800;letter-spacing:-.035em}.subtitle,.small,.section p,.hint,.statusLine{font-size:12px;color:var(--muted);line-height:1.5}.glass{background:var(--glass);border:1px solid var(--line);box-shadow:var(--shadow),inset 0 1px 0 #fff;backdrop-filter:blur(24px);-webkit-backdrop-filter:blur(24px)}.btn{border:1px solid rgba(255,255,255,.4);border-radius:999px;padding:11px 17px;background:var(--blue);color:#fff;font:inherit;font-weight:700;cursor:pointer}.btn.secondary{background:rgba(255,255,255,.62);color:var(--text);box-shadow:none}.btn:disabled{opacity:.55}.grid{display:grid;grid-template-columns:repeat(3,1fr);gap:12px}.card{border-radius:22px;padding:18px;min-height:132px}.value{font-size:19px;font-weight:800;margin-top:8px;line-height:1.25}.pill{display:inline-flex;align-items:center;gap:7px;font-size:12px;font-weight:700;margin-top:10px}.dot{width:8px;height:8px;border-radius:50%;background:var(--green);box-shadow:0 0 0 3px rgba(52,199,89,.12)}.dot.off{background:var(--red);box-shadow:0 0 0 3px rgba(255,59,48,.12)}.section{margin-top:14px;border-radius:24px;padding:20px}.sectionHead{display:flex;justify-content:space-between;gap:15px;margin-bottom:15px}.section h2{font-size:19px;margin:0}.section p{margin:7px 0}.details{display:grid;grid-template-columns:repeat(2,1fr);gap:10px}.detail,.service{padding:14px 15px;border-radius:16px;background:rgba(255,255,255,.42);border:1px solid rgba(255,255,255,.72)}.detailValue{font-size:14px;font-weight:700;margin-top:5px;word-break:break-word}.address{font-family:ui-monospace,SFMono-Regular,Consolas,monospace;font-size:13px;margin-top:7px;word-break:break-all}.selectWrap{position:relative}.selectWrap select,.field input{width:100%;padding:13px;border:1px solid rgba(255,255,255,.85);border-radius:14px;background:rgba(255,255,255,.6);color:var(--text);font:inherit;outline:none}.wifiForm{margin-top:16px;border-top:1px solid rgba(255,255,255,.7);padding-top:16px}.field{margin-bottom:12px}.field label{display:block;font-size:12px;font-weight:700;margin-bottom:6px}.check{display:flex;align-items:center;gap:8px;font-size:12px;color:var(--muted);margin:-2px 0 14px}.actions{display:flex;gap:9px;flex-wrap:wrap;align-items:center}.toast{position:fixed;left:50%;bottom:24px;transform:translate(-50%,20px);background:rgba(25,27,32,.92);color:#fff;padding:12px 16px;border-radius:999px;opacity:0;transition:.2s}.toast.show{opacity:1;transform:translate(-50%,0)}@media(max-width:760px){.grid{grid-template-columns:1fr}.details{grid-template-columns:1fr}}@media(max-width:560px){.app{padding:16px 12px 40px}.title{font-size:23px}.section{padding:16px}}
</style></head><body><main class="app"><div class="top"><div class="brand"><div class="logo">HP</div><div><div class="eyebrow">ESP32-S3 print server</div><div class="title">HP Print Server</div><div class="subtitle">Your USB printer, available over Wi-Fi</div></div></div><button class="btn secondary" id="refreshBtn" onclick="refreshStatus()">Refresh</button></div><div class="grid"><div class="card glass"><div class="label">Printer</div><div id="printerName" class="value">)HTML";
  html += esc(deviceName);
  html += R"HTML(</div><div id="printerState" class="small">)HTML";
  html += esc(printerStateText());
  html += R"HTML(</div><div id="printerReason" class="small">)HTML";
  html += esc(printerReasonText());
  html += R"HTML(</div><div id="printerPill" class="pill"><span class="dot )HTML";
  html += printerAttached ? "" : "off";
  html += R"HTML("></span><span>)HTML";
  html += printerAttached ? "Connected by USB" : "USB printer not detected";
  html += R"HTML(</span></div></div><div class="card glass"><div class="label">Wi-Fi</div><div id="wifiName" class="value">)HTML";
  html += esc(networkName);
  html += R"HTML(</div><div id="wifiState" class="small">)HTML";
  html += esc(wifiStatusText());
  html += R"HTML(</div></div><div class="card glass"><div class="label">Printing</div><div id="raw" class="value">Ready</div><div class="small">RAW / JetDirect · TCP 9100</div></div></div><div class="section glass"><div class="sectionHead"><div><h2>Connection</h2><p>Use these details to confirm the printer is online and add it on another device.</p></div></div><div class="details"><div class="detail"><div class="detailKey">USB printer</div><div id="usbDetail" class="detailValue">)HTML";
  html += printerAttached ? "Connected" : "Not detected";
  html += R"HTML(</div></div><div class="detail"><div class="detailKey">Printer status</div><div id="usbStatus" class="detailValue">)HTML";
  html += esc(usbStatusText());
  html += R"HTML(</div></div><div class="detail"><div class="detailKey">Network address</div><div id="ipDetail" class="detailValue">)HTML";
  html += esc(ip.length() ? ip : "Unavailable");
  html += R"HTML(</div></div><div class="detail"><div class="detailKey">Hostname</div><div id="hostDetail" class="detailValue">)HTML";
  html += mdnsReady ? "printer.local" : "Unavailable";
  html += R"HTML(</div></div></div></div><div class="section glass"><div class="sectionHead"><div><h2>How to print</h2><p>Add this server as a network printer using RAW / JetDirect (AppSocket). The server does not convert document formats.</p></div></div><div class="service"><strong>Printer address</strong><div id="printAddress" class="address">)HTML";
  html += mdnsReady ? "socket://printer.local:9100" : (ip.length() ? String("socket://") + ip + ":9100" : "Unavailable");
  html += R"HTML(</div><div class="small">The print stream must already be a format understood by the connected printer.</div></div></div><div class="section glass"><div class="sectionHead"><div><h2>Wi-Fi</h2><p>Change the Wi-Fi network used by the print server. Saving restarts the device.</p></div><button class="btn secondary" id="scanBtn" onclick="scanWifi()">Scan Wi-Fi</button></div><div class="selectWrap"><select id="ssidSelect"><option value="">Select a Wi-Fi network…</option></select></div><div id="scanState" class="statusLine" style="display:none"></div><form class="wifiForm" method="POST" action="/save"><div class="field"><label for="ssid">Wi-Fi network</label><input id="ssid" name="ssid" value=")HTML";
  html += esc(config.ssid);
  html += R"HTML(" maxlength="32" autocomplete="off" placeholder="Select a network or enter a hidden SSID"></div><div class="field"><label for="password">Password</label><input id="password" type="password" name="password" placeholder="Leave blank to keep the saved password"></div><label class="check"><input type="checkbox" name="clearPassword" value="1"> Clear the saved password (for an open network)</label><div class="actions"><button class="btn" type="submit">Save Wi-Fi &amp; restart</button><span class="hint">Hidden networks can be entered manually.</span></div></form></div></main><div id="toast" class="toast"></div><script>
const $=id=>document.getElementById(id);$('ssidSelect').addEventListener('change',()=>{$('ssid').value=$('ssidSelect').value;});async function scanWifi(){const b=$('scanBtn'),select=$('ssidSelect');b.disabled=true;b.textContent='Scanning…';select.innerHTML='<option value="">Scanning…</option>';try{const r=await fetch('/scan.json?ts='+Date.now());if(!r.ok)throw new Error();const a=await r.json();a.sort((x,y)=>y.rssi-x.rssi);select.innerHTML='<option value="">Select a Wi-Fi network…</option>';a.forEach(x=>{const o=document.createElement('option');o.value=x.ssid;o.textContent=x.ssid+' · '+x.rssi+' dBm · Ch '+x.channel;select.appendChild(o);});$('scanState').style.display='block';$('scanState').textContent=a.length?a.length+' nearby Wi-Fi network'+(a.length===1?'':'s')+' found.':'No visible Wi-Fi networks found.';}catch(e){select.innerHTML='<option value="">Scan failed</option>';$('scanState').style.display='block';$('scanState').textContent='Wi-Fi scan failed.';}finally{b.disabled=false;b.textContent='Scan Wi-Fi';}}async function refreshStatus(){const b=$('refreshBtn');b.disabled=true;b.textContent='Refreshing…';try{const r=await fetch('/status.json?ts='+Date.now());if(!r.ok)throw new Error();const s=await r.json();$('printerName').textContent=s.usbAttached?s.printerName:'No USB printer';$('printerState').textContent=s.printer;$('printerReason').textContent=s.printerReason;$('wifiName').textContent=s.ssid||'Not connected';$('wifiState').textContent=s.wifi;$('raw').textContent=s.rawConnected?'Print job active':'Ready';$('usbDetail').textContent=s.usbAttached?'Connected':'Not detected';$('usbStatus').textContent=s.usbStatus;$('ipDetail').textContent=s.ip||'Unavailable';$('hostDetail').textContent=s.mdnsReady?'printer.local':'Unavailable';$('printAddress').textContent=s.mdnsReady?'socket://printer.local:9100':(s.ip?'socket://'+s.ip+':9100':'Unavailable');$('printerPill').innerHTML='<span class="dot '+(s.usbAttached?'':'off')+'"></span><span>'+(s.usbAttached?'Connected by USB':'USB printer not detected')+'</span>';}catch(e){showToast('Status refresh failed');}finally{b.disabled=false;b.textContent='Refresh';}}function showToast(t){const x=$('toast');x.textContent=t;x.classList.add('show');clearTimeout(window._toast);window._toast=setTimeout(()=>x.classList.remove('show'),2800);}setInterval(refreshStatus,5000);
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
  Serial.println("=== ESP32-S3 RAW 9100 USB Print Server ===");
  Serial.println("[MODE] RAW JetDirect/AppSocket only; classic USB Printer Class is selected automatically");

  loadConfig();
  if (!connectWiFi()) startConfigAP();
  if (WiFi.status() == WL_CONNECTED) startRawDiscovery();

  usbPrinterBackend.begin();

  configServer.on("/", HTTP_GET, handleRoot);
  configServer.on("/scan.json", HTTP_GET, sendJsonScan);
  configServer.on("/status.json", HTTP_GET, sendJsonStatus);
  configServer.on("/save", HTTP_POST, handleSave);
  configServer.begin();

  Serial.println("[HTTP] Configuration server ready");
  Serial.printf("[HTTP] Open http://%s\n",
                mdnsReady ? "printer.local" : (activeIp().length() ? activeIp().c_str() : "device-address"));
  Serial.println("[RAW] TCP 9100 server enabled");
}

void loop() {
  configServer.handleClient();
  maintainWiFi();
  usbHost.poll();
  usbPrinterBackend.poll();

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
