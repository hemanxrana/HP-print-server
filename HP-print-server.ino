#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include "usb_printer_backend.h"
#include "ipp_pcl3_service.h"
#include "system_diagnostics.h"
#include "remote_diagnostics.h"
#include <ArduinoOTA.h>
#include <esp_ota_ops.h>
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"
#if __has_include(<mdns.h>)
#include <mdns.h>
#endif

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

struct Config { String ssid; String password; };
Config config;
static unsigned long lastStatus = 0;
static bool mdnsReady = false;
static bool configApActive = false;
static bool otaReady = false;
static bool otaInProgress = false;
static constexpr const char *OTA_PASSWORD = "change-this-ota-password";

String esc(String s) { s.replace("&", "&amp;"); s.replace("<", "&lt;"); s.replace(">", "&gt;"); s.replace("\"", "&quot;"); s.replace("'", "&#39;"); return s; }
String jsonEsc(String s) { s.replace("\\", "\\\\"); s.replace("\"", "\\\""); s.replace("\r", "\\r"); s.replace("\n", "\\n"); return s; }
void defaults() { config.ssid = ""; config.password = ""; }
void loadConfig() { defaults(); if (!preferences.begin(CONFIG_NS, true)) { Serial.println("[CFG] Preferences read failed; using defaults"); return; } config.ssid = preferences.getString("ssid", config.ssid); config.password = preferences.getString("pass", config.password); preferences.end(); }
bool saveConfig() { if (!preferences.begin(CONFIG_NS, false)) return false; const size_t ssidWritten = preferences.putString("ssid", config.ssid); const size_t passWritten = preferences.putString("pass", config.password); preferences.end(); return (config.ssid.isEmpty() || ssidWritten > 0) && (config.password.isEmpty() || passWritten > 0); }

bool connectWiFi() {
  if (config.ssid.isEmpty()) { Serial.println("[WiFi] No saved SSID"); return false; }
  WiFi.mode(WIFI_STA); WiFi.setSleep(false); WiFi.setHostname(RAW_HOSTNAME); WiFi.begin(config.ssid.c_str(), config.password.c_str()); Serial.printf("[WiFi] Connecting to %s\n", config.ssid.c_str());
  const unsigned long started = millis(); while (WiFi.status() != WL_CONNECTED && millis() - started < 20000UL) { delay(250); Serial.print('.'); } Serial.println();
  if (WiFi.status() != WL_CONNECTED) { Serial.printf("[WiFi] Connection failed, status=%d\n", (int)WiFi.status()); WiFi.disconnect(false, false); return false; }
  Serial.printf("[WiFi] Connected: %s\n", WiFi.localIP().toString().c_str()); return true;
}

void startOtaIfPossible() {
  if (otaReady || WiFi.status() != WL_CONNECTED) return;
  ArduinoOTA.setHostname("hp-print-server");
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([] {
    otaInProgress = true;
    Serial.println("[OTA] Update started; printing is paused");
    RemoteDiagnostics::publish("OTA update started\n");
  });
  ArduinoOTA.onEnd([] {
    Serial.println("[OTA] Update complete; rebooting");
    RemoteDiagnostics::publish("OTA update complete; rebooting\n");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    static unsigned int lastPercent = 101;
    const unsigned int percent = total ? (progress * 100U) / total : 0;
    if (percent != lastPercent && (percent % 10U == 0 || percent == 100U)) {
      lastPercent = percent;
      Serial.printf("[OTA] Progress %u%%\n", percent);
      RemoteDiagnostics::publishf("OTA progress %u%%\n", percent);
    }
  });
  ArduinoOTA.onError([](ota_error_t error) {
    otaInProgress = false;
    Serial.printf("[OTA] Error %u\n", (unsigned)error);
    RemoteDiagnostics::publishf("OTA failed: error %u\n", (unsigned)error);
  });
  ArduinoOTA.begin();
  otaReady = true;
  Serial.println("[OTA] Wireless upload ready");
  RemoteDiagnostics::publish("OTA ready; select hp-print-server in Arduino IDE\n");
}

bool startConfigAP() {
  WiFi.mode(WIFI_AP_STA); WiFi.setSleep(false); WiFi.setHostname(RAW_HOSTNAME);
  if (!WiFi.softAP(AP_SSID, AP_PASSWORD, 1, false, 4)) { Serial.println("[AP] Failed to start configuration AP"); configApActive = false; return false; }
  configApActive = true; Serial.printf("[AP] SSID: %s\n", AP_SSID); Serial.printf("[AP] Configure at http://%s\n", WiFi.softAPIP().toString().c_str()); return true;
}

void startRawDiscovery() {
  MDNS.end(); mdnsReady = false;
  if (!MDNS.begin(RAW_HOSTNAME)) { Serial.println("[mDNS] Failed to start printer.local discovery responder"); return; }
  mdnsReady = true;
  MDNS.setInstanceName("HP Smart Tank");
  if (MDNS.addService("pdl-datastream", "tcp", 9100)) {
    MDNS.addServiceTxt("pdl-datastream", "tcp", "txtvers", "1");
    MDNS.addServiceTxt("pdl-datastream", "tcp", "note", "RAW 9100");
    Serial.println("[mDNS] printer.local -> RAW 9100 discovery advertised");
  }
  if (MDNS.addService("ipp", "tcp", 631)) {
    MDNS.addServiceTxt("ipp", "tcp", "txtvers", "1");
    MDNS.addServiceTxt("ipp", "tcp", "qtotal", "1");
    MDNS.addServiceTxt("ipp", "tcp", "rp", "ipp/print");
    MDNS.addServiceTxt("ipp", "tcp", "ty", "HP Smart Tank");
    MDNS.addServiceTxt("ipp", "tcp", "product", "(HP Smart Tank)");
    MDNS.addServiceTxt("ipp", "tcp", "pdl", "application/vnd.hp-pcl");
    MDNS.addServiceTxt("ipp", "tcp", "usb_MFG", "HP");
    MDNS.addServiceTxt("ipp", "tcp", "usb_MDL", "HP Smart Tank 520_540 series");
    MDNS.addServiceTxt("ipp", "tcp", "usb_CMD", "PCL3GUI");
    MDNS.addServiceTxt("ipp", "tcp", "adminurl", (String("http://") + (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : WiFi.softAPIP().toString()) + "/").c_str());
    MDNS.addServiceTxt("ipp", "tcp", "printer-state", "3");
    MDNS.addServiceTxt("ipp", "tcp", "note", "RAW 9100 + IPP PCL3GUI");
#if __has_include(<mdns.h>)
    mdns_service_subtype_add_for_host(nullptr, "_ipp", "_tcp", nullptr, "_universal");
#endif
    Serial.println("[mDNS] HP Smart Tank -> IPP 631 advertised as PCL3GUI only");
  }
}

String printerStateText() { switch (usbPrinterBackend.state()) { case UsbPrinterBackend::OFFLINE: return "Offline"; case UsbPrinterBackend::IDLE: return "Ready"; case UsbPrinterBackend::PRINTING: return "Printing"; case UsbPrinterBackend::ERROR: return "Printer error"; } return "Unknown"; }
String printerReasonText() { const String &reason = usbPrinterBackend.statusReason(); if (reason == "waiting-for-usb-printer") return "Connect a compatible USB printer"; if (reason == "enumerating-usb-device") return "Checking the USB printer"; if (reason == "printer-interface-ready" || reason == "usb-printer-ready") return "Printer is ready"; if (reason == "raw-job-in-progress") return "Sending the print job"; if (reason == "raw-job-draining") return "Finishing the print job"; if (reason == "usb-printer-reports-paper-empty") return "Paper is empty or unavailable"; if (reason == "usb-printer-reports-not-selected") return "Printer is not selected or not ready"; if (reason == "usb-printer-reports-error" || reason == "usb-printer-reports-error-after-job") return "Printer reported an error"; if (reason == "selected-interface-has-no-bulk-output") return "No compatible RAW printing interface was found"; if (reason.startsWith("USB bulk transfer failed") || reason.startsWith("USB Bulk OUT")) return "USB transfer to the printer failed"; return reason.length() ? reason : "Waiting for printer"; }
String usbStatusText() { if (!usbHost.device().attached) return "No USB printer detected"; if (!usbHost.portStatusValid()) return "Printer connected; detailed status is not available yet"; if (usbHost.portStatusError()) return "Printer reports an error"; if (usbHost.portStatusPaperEmpty()) return "Paper is empty or unavailable"; if (!usbHost.portStatusSelected()) return "Printer is not selected or not ready"; return "Printer is selected and ready"; }
String wifiStatusText() { if (WiFi.status() == WL_CONNECTED) return String("Connected · ") + WiFi.localIP().toString(); if (configApActive) return String("Setup access point · ") + WiFi.softAPIP().toString(); return "Not connected"; }
String activeNetworkName() { if (WiFi.status() == WL_CONNECTED) return WiFi.SSID(); if (configApActive) return String(AP_SSID); return "Not connected"; }
String activeIp() { if (WiFi.status() == WL_CONNECTED) return WiFi.localIP().toString(); if (configApActive) return WiFi.softAPIP().toString(); return ""; }

static int lastWifiStatus = -1;
static unsigned long lastWifiAttemptMs = 0;
static uint32_t bootCount = 0;
static bool otaValidated = false;

void sendJsonScan() { const int n = WiFi.scanNetworks(false, true); String out = "["; bool first = true; for (int i = 0; i < n; ++i) { String ssid = WiFi.SSID(i); if (ssid.isEmpty()) continue; if (!first) out += ","; first = false; out += String("{\"ssid\":\"") + jsonEsc(ssid) + "\",\"rssi\":" + String(WiFi.RSSI(i)) + ",\"channel\":" + String(WiFi.channel(i)) + "}"; } out += "]"; WiFi.scanDelete(); configServer.send(200, "application/json", out); }
void sendJsonStatus() { const bool printerAttached = usbHost.device().attached; const String printerName = printerAttached && usbHost.device().product.length() ? usbHost.device().product : "USB printer"; String out; out.reserve(1600); out += "{\"printer\":\"" + jsonEsc(printerStateText()); out += "\",\"printerReason\":\"" + jsonEsc(printerReasonText()); out += "\",\"printerName\":\"" + jsonEsc(printerName); out += "\",\"usbStatus\":\"" + jsonEsc(usbStatusText()); out += "\",\"wifi\":\"" + jsonEsc(wifiStatusText()); out += "\",\"ssid\":\"" + jsonEsc(activeNetworkName()); out += "\",\"rawConnected\":" + String(usbPrinterBackend.rawClientConnected() ? "true" : "false"); out += ",\"usbAttached\":" + String(printerAttached ? "true" : "false");   out += ",\"ip\":\"" + jsonEsc(activeIp()); out += "\",\"mdnsReady\":" + String(mdnsReady ? "true" : "false"); out += ",\"hostname\":\"" + String(mdnsReady ? "printer.local" : "") + "\"";
  out += ",\"jobsOk\":" + String(usbPrinterBackend.jobsCompleted()) + ",\"jobsFailed\":" + String(usbPrinterBackend.jobsFailed());
  out += ",\"maxDrainMs\":" + String(usbPrinterBackend.maxDrainMs()) + ",\"lastError\":\"" + jsonEsc(usbPrinterBackend.lastError()) + "\"";
  out += ",\"ippJob\":" + String(ippService.lastJobId()) + ",\"ippState\":" + String(ippService.lastJobState()) + ",\"ippReason\":\"" + jsonEsc(ippService.lastJobReason()) + "\"";
  out += ",\"freeHeap\":" + String(ESP.getFreeHeap()) + ",\"boots\":" + String(bootCount) + ",\"usbWedged\":" + String(usbHost.bulkOutWedged() ? "true" : "false");
  out += ",\"psramTotal\":" + String((unsigned)ESP.getPsramSize()) + ",\"psramFree\":" + String((unsigned)ESP.getFreePsram()) + ",\"logInPsram\":" + String(RemoteDiagnostics::logInPsram() ? "true" : "false");
  out += "}"; configServer.send(200, "application/json", out); }

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
  out.append(R"HTML(<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><meta name="theme-color" content="#f3f5f7"><title>Print Server</title><style>*{box-sizing:border-box}body{margin:0;background:#f3f5f7;color:#344054;font-family:system-ui,-apple-system,"Segoe UI",Roboto,Arial,sans-serif;font-size:14px;line-height:1.5}.app{max-width:940px;margin:auto;padding:22px 16px 40px}.top{display:flex;align-items:center;justify-content:space-between;gap:14px;margin-bottom:16px}.title{font-size:25px;font-weight:650}.subtitle,.small,.hint,.statusLine,.section p{color:#758195;font-size:13px}.actions{display:flex;gap:8px;align-items:center;flex-wrap:wrap}.btn{border:1px solid #d8e1e8;border-radius:9px;padding:8px 12px;background:#edf3f7;color:#466681;font:inherit;font-weight:600;cursor:pointer}.btn.primary{background:#557b9a;color:#fff;border-color:#557b9a}.grid{display:grid;grid-template-columns:repeat(3,1fr);gap:10px}.card,.section{background:#fbfcfd;border:1px solid #e1e7ec;border-radius:14px}.card{padding:15px;min-height:112px}.section{margin-top:11px;padding:17px}.label,.detailKey{font-size:11px;text-transform:uppercase;color:#7b8797;font-weight:600}.value{font-size:18px;font-weight:650;margin-top:6px}.pill{display:flex;align-items:center;gap:7px;font-size:12px;color:#637083;margin-top:8px}.dot{width:8px;height:8px;border-radius:50%;background:#719b7d}.dot.off{background:#b47b72}.sectionHead{display:flex;justify-content:space-between;gap:12px;margin-bottom:12px}.section h2{font-size:17px;margin:0}.details{display:grid;grid-template-columns:repeat(2,1fr);gap:8px}.detail,.service{padding:11px 12px;border-radius:10px;background:#f6f8fa;border:1px solid #e7ebef}.detailValue{font-size:13px;font-weight:600;margin-top:3px}.address{font-family:ui-monospace,SFMono-Regular,Consolas,monospace;font-size:12px;margin-top:6px;color:#536272}.selectWrap select,.field input{width:100%;padding:10px 11px;border:1px solid #dce3e9;border-radius:9px;background:#fff;color:#344054;font:inherit}.wifiForm{margin-top:13px;border-top:1px solid #e7ebef;padding-top:13px}.field{margin-bottom:10px}.field label{display:block;font-size:12px;font-weight:600;margin-bottom:5px}.check{display:flex;align-items:center;gap:8px;font-size:12px;color:#758195;margin:0 0 12px}a.btn{text-decoration:none}@media(max-width:720px){.grid{grid-template-columns:1fr}.details{grid-template-columns:1fr}}</style></head><body><main class="app"><div class="top"><div><div class="title">HP Smart Tank</div><div class="subtitle">USB printer over Wi-Fi</div></div><div class="actions"><a class="btn" href="/scan">Scanner (disabled)</a><button class="btn" id="refreshBtn" onclick="refreshStatus()">Refresh</button></div></div><div class="grid"><div class="card"><div class="label">Printer</div><div id="printerName" class="value">)HTML");
  out.appendEscaped(deviceName);
  out.append(R"HTML(</div><div id="printerState" class="small">)HTML"); out.appendEscaped(printerStateText()); out.append(R"HTML(</div><div id="printerReason" class="small">)HTML"); out.appendEscaped(printerReasonText()); out.append(R"HTML(</div><div id="printerPill" class="pill"><span class="dot )HTML"); out.append(printerAttached ? "" : "off"); out.append(R"HTML("></span><span>)HTML"); out.append(printerAttached ? "USB connected" : "USB not detected"); out.append(R"HTML(</span></div></div><div class="card"><div class="label">Network</div><div id="wifiName" class="value">)HTML"); out.appendEscaped(networkName); out.append(R"HTML(</div><div id="wifiState" class="small">)HTML"); out.appendEscaped(wifiStatusText()); out.append(R"HTML(</div></div><div class="card"><div class="label">RAW printing</div><div id="raw" class="value">)HTML"); out.append(usbPrinterBackend.rawClientConnected() ? "Active" : (usbPrinterBackend.online() ? "Ready" : "Unavailable"));
  out.append(R"HTML(</div><div class="small">JetDirect / AppSocket · TCP 9100</div></div></div><div class="section"><div class="sectionHead"><div><h2>Connection</h2><p>Essential device and network details.</p></div></div><div class="details"><div class="detail"><div class="detailKey">USB</div><div id="usbDetail" class="detailValue">)HTML"); out.append(printerAttached ? "Connected" : "Not detected"); out.append(R"HTML(</div></div><div class="detail"><div class="detailKey">Printer status</div><div id="usbStatus" class="detailValue">)HTML"); out.appendEscaped(usbStatusText()); out.append(R"HTML(</div></div><div class="detail"><div class="detailKey">IP address</div><div id="ipDetail" class="detailValue">)HTML"); out.appendEscaped(ip.length() ? ip : "Unavailable"); out.append(R"HTML(</div></div><div class="detail"><div class="detailKey">Hostname</div><div id="hostDetail" class="detailValue">)HTML"); out.append(mdnsReady ? "printer.local" : "Unavailable"); out.append(R"HTML(</div></div></div></div><div class="section"><div class="sectionHead"><div><h2>Print addresses</h2><p>RAW remains available; IPP advertises only HP PCL3GUI.</p></div></div><div class="service"><div id="printAddress" class="address">)HTML"); out.append((mdnsReady ? "socket://printer.local:9100" : (ip.length() ? String("socket://") + ip + ":9100" : "Unavailable")).c_str()); out.append(R"HTML(</div></div><div class="service" style="margin-top:8px"><div class="label">IPP · PCL3GUI only</div><div class="address">ipp://printer.local:631/ipp/print</div></div></div><div class="section"><div class="sectionHead"><div><h2>Wi-Fi</h2><p>Change the network used by this print server.</p></div><button class="btn" id="scanBtn" onclick="scanWifi()">Scan networks</button></div><div class="selectWrap"><select id="ssidSelect"><option value="">Select a Wi-Fi network…</option></select></div><div id="scanState" class="statusLine" style="display:none"></div><form class="wifiForm" method="POST" action="/save"><div class="field"><label for="ssid">Wi-Fi network</label><input id="ssid" name="ssid" value=")HTML"); out.appendEscaped(config.ssid); out.append(R"HTML(" maxlength="32" autocomplete="off" placeholder="Select a network or enter a hidden SSID"></div><div class="field"><label for="password">Password</label><input id="password" type="password" name="password" placeholder="Leave blank to keep the saved password"></div><label class="check"><input type="checkbox" name="clearPassword" value="1"> Clear saved password for an open network</label><div class="actions"><button class="btn primary" type="submit">Save &amp; restart</button></div></form></div></main><script>const $=id=>document.getElementById(id);$('ssidSelect').addEventListener('change',()=>{$('ssid').value=$('ssidSelect').value;});async function scanWifi(){const b=$('scanBtn'),select=$('ssidSelect');b.disabled=true;select.innerHTML='<option value="">Scanning…</option>';try{const r=await fetch('/scan.json?ts='+Date.now());const a=await r.json();a.sort((x,y)=>y.rssi-x.rssi);select.innerHTML='<option value="">Select a Wi-Fi network…</option>';a.forEach(x=>{const o=document.createElement('option');o.value=x.ssid;o.textContent=x.ssid+' · '+x.rssi+' dBm · Ch '+x.channel;select.appendChild(o);});}finally{b.disabled=false;}}async function refreshStatus(){try{const r=await fetch('/status.json?ts='+Date.now());const s=await r.json();$('printerName').textContent=s.usbAttached?s.printerName:'No USB printer';$('printerState').textContent=s.printer;$('printerReason').textContent=s.printerReason;$('wifiName').textContent=s.ssid||'Not connected';$('wifiState').textContent=s.wifi;$('raw').textContent=s.rawConnected?'Active':(s.printer==='Ready'?'Ready':'Unavailable');$('usbDetail').textContent=s.usbAttached?'Connected':'Not detected';$('usbStatus').textContent=s.usbStatus;$('ipDetail').textContent=s.ip||'Unavailable';$('hostDetail').textContent=s.mdnsReady?'printer.local':'Unavailable';$('printAddress').textContent=s.mdnsReady?'socket://printer.local:9100':(s.ip?'socket://'+s.ip+':9100':'Unavailable');}catch(e){}}setInterval(refreshStatus,5000);</script></body></html>)HTML");
  return out.c_str();
}

void handleRoot() { const char *page = dashboard(); const size_t length = strlen(page); configServer.setContentLength(length); configServer.send(200, "text/html; charset=utf-8"); if (length) configServer.sendContent(page, length); }
void handleSave() { if (!configServer.hasArg("ssid") || configServer.arg("ssid").isEmpty()) { configServer.send(400, "text/plain; charset=utf-8", "Wi-Fi network name is required."); return; } config.ssid = configServer.arg("ssid"); if (configServer.hasArg("clearPassword")) config.password = ""; else if (configServer.hasArg("password") && !configServer.arg("password").isEmpty()) config.password = configServer.arg("password"); if (!saveConfig()) { configServer.send(500, "text/plain; charset=utf-8", "Could not save Wi-Fi settings. The device was not restarted."); return; } configServer.send(200, "text/html; charset=utf-8", "<p>Wi-Fi settings saved. Rebooting…</p>"); delay(300); ESP.restart(); }

void loadBootCount() {
  if (!preferences.begin("hp-diag", false)) return;
  bootCount = preferences.getUInt("boots", 0) + 1;
  preferences.putUInt("boots", bootCount);
  preferences.end();
}

void startWatchdog() {
  esp_task_wdt_config_t cfg = {};
  cfg.timeout_ms = 60000;
  cfg.idle_core_mask = 0;
  cfg.trigger_panic = true;
  if (esp_task_wdt_init(&cfg) != ESP_OK) esp_task_wdt_reconfigure(&cfg);
  esp_task_wdt_add(nullptr);
  Serial.println("[WDT] Task watchdog armed (60 s)");
}

void validateOtaImage() {
  if (otaValidated || millis() < 10000UL) return;
  otaValidated = true;
  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  if (running && esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
      Serial.println("[OTA] Pending image marked valid; rollback cancelled");
    }
  }
}

void handleLog() { size_t length = 0; const char *ring = RemoteDiagnostics::logData(length); configServer.setContentLength(length); configServer.send(200, "text/plain; charset=utf-8"); if (length) configServer.sendContent(ring, length); }

void diagCommand(const String &command) {
  if (command == "log") {
    const String logText = RemoteDiagnostics::recentLog();
    RemoteDiagnostics::publish(logText.length() ? logText : String("log is empty\n"));
  } else if (command == "counters") {
    RemoteDiagnostics::publishf("jobs-ok=%lu jobs-failed=%lu max-drain=%lu ms boots=%lu free-heap=%lu psram-free=%lu psram-total=%lu rssi=%d\n",
                                (unsigned long)usbPrinterBackend.jobsCompleted(),
                                (unsigned long)usbPrinterBackend.jobsFailed(),
                                (unsigned long)usbPrinterBackend.maxDrainMs(),
                                (unsigned long)bootCount,
                                (unsigned long)ESP.getFreeHeap(),
                                (unsigned long)ESP.getFreePsram(),
                                (unsigned long)ESP.getPsramSize(),
                                (int)WiFi.RSSI());
  } else if (command == "job") {
    RemoteDiagnostics::publishf("ipp-job=%lu ipp-state=%u last-error=%s\n",
                                (unsigned long)ippService.lastJobId(),
                                (unsigned)ippService.lastJobState(),
                                usbPrinterBackend.lastError().length() ? usbPrinterBackend.lastError().c_str() : "none");
  } else {
    RemoteDiagnostics::publish("unknown command; use help\n");
  }
}

void onWifiConnected() {
  if (configApActive) {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    configApActive = false;
    Serial.println("[AP] Station link up; configuration AP stopped");
  }
  startRawDiscovery();
  usbPrinterBackend.restartRawListener();
  ippService.rebind();
  if (!otaReady) startOtaIfPossible();
  Serial.println("[WiFi] Station link ready; RAW/IPP listeners re-bound");
}

void serviceWifi() {
  const int status = (int)WiFi.status();
  if (status == WL_CONNECTED && lastWifiStatus != WL_CONNECTED) {
    onWifiConnected();
  } else if (status != WL_CONNECTED && lastWifiStatus == WL_CONNECTED) {
    Serial.println("[WiFi] Station link lost; retrying in background");
  }

  if (status != WL_CONNECTED && !config.ssid.isEmpty() && millis() - lastWifiAttemptMs >= 30000UL) {
    lastWifiAttemptMs = millis();
    Serial.printf("[WiFi] Retrying saved network %s\n", config.ssid.c_str());
    WiFi.reconnect();
  }
  lastWifiStatus = status;
}

void setup() {
  Serial.begin(115200); delay(500); Serial.println(); SystemDiagnostics::logBoot(); Serial.println("=== ESP32-S3 HP PCL3GUI Print Server ===");
  loadBootCount();
  startWatchdog();
  RemoteDiagnostics::begin();
  RemoteDiagnostics::setCommandHook(diagCommand);
  Serial.printf("[MEM] internal-heap-free=%lu psram-total=%u psram-free=%u\n",
                (unsigned long)ESP.getFreeHeap(), (unsigned)ESP.getPsramSize(),
                (unsigned)ESP.getFreePsram());
  Serial.println("[MODE] RAW 9100 + IPP 631 PCL3GUI; both print through the classic USB Printer Class interface");
  Serial.println("[MODE] IPP-over-USB printing is disabled/not used; scanner USB backend is disabled");
  loadConfig(); if (!connectWiFi()) startConfigAP(); startRawDiscovery(); startOtaIfPossible();
  lastWifiStatus = (int)WiFi.status();
  usbPrinterBackend.begin(); ippService.begin();
  configServer.on("/", HTTP_GET, handleRoot); configServer.on("/scan.json", HTTP_GET, sendJsonScan); configServer.on("/status.json", HTTP_GET, sendJsonStatus); configServer.on("/log", HTTP_GET, handleLog); configServer.on("/save", HTTP_POST, handleSave); ensureUsbScannerWebRoutesInstalled(); configServer.begin();
  Serial.println("[HTTP] Configuration server ready"); Serial.printf("[HTTP] Open http://%s\n", mdnsReady ? "printer.local" : (activeIp().length() ? activeIp().c_str() : "device-address")); Serial.println("[RAW] TCP 9100 server enabled"); Serial.println("[IPP] TCP 631 service enabled; document-format-supported=application/vnd.hp-pcl; version=PCL3GUI");
}

void loop() {
  esp_task_wdt_reset();
  serviceWifi();
  configServer.handleClient();
  RemoteDiagnostics::poll();
  startOtaIfPossible();
  validateOtaImage();
  if (otaReady && usbPrinterBackend.state() != UsbPrinterBackend::PRINTING && !usbPrinterBackend.rawClientConnected()) ArduinoOTA.handle();
  if (!otaInProgress) {
    usbHost.poll(); usbPrinterBackend.poll(); ippService.poll();
  }
  if (millis() - lastStatus > 1000) {
    lastStatus = millis(); const int wifiState = (int)WiFi.status(); const String ip = activeIp(); const int usbState = (int)usbHost.state(); const String printerState = printerStateText(); const bool rawConnected = usbPrinterBackend.rawClientConnected(); const bool usbStatusValid = usbHost.portStatusValid(); const uint8_t usbPort = usbStatusValid ? usbHost.portStatusValue() : 0;
    static bool initialized = false; static int lastWifiState = -1; static String lastIp; static int lastUsbState = -1; static String lastPrinterState; static bool lastRawConnected = false; static bool lastUsbStatusValid = false; static uint8_t lastUsbPort = 0;
    const bool changed = !initialized || wifiState != lastWifiState || ip != lastIp || usbState != lastUsbState || printerState != lastPrinterState || rawConnected != lastRawConnected || usbStatusValid != lastUsbStatusValid || (usbStatusValid && usbPort != lastUsbPort);
    if (changed) { Serial.printf("[STATUS] WiFi=%d IP=%s USB=%d printer=%s raw=%s%s\n", wifiState, ip.c_str(), usbState, printerState.c_str(), rawConnected ? "connected" : "idle", usbStatusValid ? (String(" usbport=0x") + String(usbPort, HEX)).c_str() : " usbstatus=unavailable"); RemoteDiagnostics::publishf("[STATUS] WiFi=%d IP=%s USB=%d printer=%s raw=%s\n", wifiState, ip.c_str(), usbState, printerState.c_str(), rawConnected ? "connected" : "idle"); lastWifiState = wifiState; lastIp = ip; lastUsbState = usbState; lastPrinterState = printerState; lastRawConnected = rawConnected; lastUsbStatusValid = usbStatusValid; lastUsbPort = usbPort; initialized = true; }
  }
}
