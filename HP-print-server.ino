#include <Arduino.h>
#include "logger.h"
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
WiFiServer monitorServer(23);
WiFiClient monitorClient;
uint32_t monitorSequence = 0;
UsbHostManager usbHost;
UsbPrinterBackend usbPrinterBackend(usbHost);
IppPcl3Service ippService(usbPrinterBackend);

void ensureUsbScannerWebRoutesInstalled();

struct Config { String ssid; String password; bool bleEnabled = true; String bleName = "HP-Print-Diagnostics"; String otaPassword = "change-this-ota-password"; };
Config config;
static unsigned long lastStatus = 0;
static bool mdnsReady = false;
static bool configApActive = false;
static bool otaReady = false;
static bool otaInProgress = false;

String esc(String s) { s.replace("&", "&amp;"); s.replace("<", "&lt;"); s.replace(">", "&gt;"); s.replace("\"", "&quot;"); s.replace("'", "&#39;"); return s; }
String jsonEsc(String s) { s.replace("\\", "\\\\"); s.replace("\"", "\\\""); s.replace("\r", "\\r"); s.replace("\n", "\\n"); return s; }
void defaults() { config.ssid = ""; config.password = ""; config.bleEnabled = true; config.bleName = "HP-Print-Diagnostics"; config.otaPassword = "change-this-ota-password"; }
void loadConfig() { defaults(); if (!preferences.begin(CONFIG_NS, true)) { Log.println("[CFG] Preferences read failed; using defaults"); return; } config.ssid = preferences.getString("ssid", config.ssid); config.password = preferences.getString("pass", config.password); config.bleEnabled = preferences.getBool("ble_en", config.bleEnabled); config.bleName = preferences.getString("ble_name", config.bleName); config.otaPassword = preferences.getString("ota_pass", config.otaPassword); preferences.end(); }
bool saveConfig() { if (!preferences.begin(CONFIG_NS, false)) return false; const size_t ssidWritten = preferences.putString("ssid", config.ssid); const size_t passWritten = preferences.putString("pass", config.password); const size_t bleWritten = preferences.putBool("ble_en", config.bleEnabled); const size_t bleNameWritten = preferences.putString("ble_name", config.bleName); const size_t otaWritten = preferences.putString("ota_pass", config.otaPassword); preferences.end(); return (config.ssid.isEmpty() || ssidWritten > 0) && (config.password.isEmpty() || passWritten > 0) && bleWritten > 0 && bleNameWritten > 0 && otaWritten > 0; }

bool connectWiFi() {
  if (config.ssid.isEmpty()) { Log.println("[WiFi] No saved SSID"); return false; }
  WiFi.mode(WIFI_STA); WiFi.setSleep(false); WiFi.setHostname(RAW_HOSTNAME); WiFi.begin(config.ssid.c_str(), config.password.c_str()); Log.printf("[WiFi] Connecting to %s\n", config.ssid.c_str());
  const unsigned long started = millis(); while (WiFi.status() != WL_CONNECTED && millis() - started < 20000UL) { delay(250); Log.print('.'); } Log.println();
  if (WiFi.status() != WL_CONNECTED) { Log.printf("[WiFi] Connection failed, status=%d\n", (int)WiFi.status()); WiFi.disconnect(false, false); return false; }
  Log.printf("[WiFi] Connected: %s\n", WiFi.localIP().toString().c_str()); return true;
}

void startOtaIfPossible() {
  if (otaReady || WiFi.status() != WL_CONNECTED) return;
  ArduinoOTA.setHostname("hp-print-server");
  ArduinoOTA.setPassword(config.otaPassword);
  ArduinoOTA.onStart([] {
    otaInProgress = true;
    Log.println("[OTA] Update started; printing is paused");
    RemoteDiagnostics::publish("OTA update started\n");
  });
  ArduinoOTA.onEnd([] {
    Log.println("[OTA] Update complete; rebooting");
    RemoteDiagnostics::publish("OTA update complete; rebooting\n");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    static unsigned int lastPercent = 101;
    const unsigned int percent = total ? (progress * 100U) / total : 0;
    if (percent != lastPercent && (percent % 10U == 0 || percent == 100U)) {
      lastPercent = percent;
      Log.printf("[OTA] Progress %u%%\n", percent);
      RemoteDiagnostics::publishf("OTA progress %u%%\n", percent);
    }
  });
  ArduinoOTA.onError([](ota_error_t error) {
    otaInProgress = false;
    Log.printf("[OTA] Error %u\n", (unsigned)error);
    RemoteDiagnostics::publishf("OTA failed: error %u\n", (unsigned)error);
  });
  ArduinoOTA.begin();
  otaReady = true;
  Log.println("[OTA] Wireless upload ready");
  RemoteDiagnostics::publish("OTA ready; select hp-print-server in Arduino IDE\n");
}

bool startConfigAP() {
  WiFi.mode(WIFI_AP_STA); WiFi.setSleep(false); WiFi.setHostname(RAW_HOSTNAME);
  if (!WiFi.softAP(AP_SSID, AP_PASSWORD, 1, false, 4)) { Log.println("[AP] Failed to start configuration AP"); configApActive = false; return false; }
  configApActive = true; Log.printf("[AP] SSID: %s\n", AP_SSID); Log.printf("[AP] Configure at http://%s\n", WiFi.softAPIP().toString().c_str()); return true;
}

void startRawDiscovery() {
  MDNS.end(); mdnsReady = false;
  if (!MDNS.begin(RAW_HOSTNAME)) { Log.println("[mDNS] Failed to start printer.local discovery responder"); return; }
  mdnsReady = true;
  MDNS.setInstanceName("HP Smart Tank");
  if (MDNS.addService("pdl-datastream", "tcp", 9100)) {
    MDNS.addServiceTxt("pdl-datastream", "tcp", "txtvers", "1");
    MDNS.addServiceTxt("pdl-datastream", "tcp", "note", "RAW 9100");
    Log.println("[mDNS] printer.local -> RAW 9100 discovery advertised");
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
    Log.println("[mDNS] HP Smart Tank -> IPP 631 advertised as PCL3GUI only");
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
  out += ",\"bleEnabled\":" + String(RemoteDiagnostics::enabled() ? "true" : "false");
  out += ",\"bleReady\":" + String(RemoteDiagnostics::ready() ? "true" : "false");
  out += ",\"bleConnected\":" + String(RemoteDiagnostics::connected() ? "true" : "false");
  out += ",\"bleName\":\"" + jsonEsc(RemoteDiagnostics::deviceName()) + "\"";
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
  const bool bleEnabled = RemoteDiagnostics::enabled();
  const bool bleReady = RemoteDiagnostics::ready();
  const bool bleConnected = RemoteDiagnostics::connected();
  const String bleName = RemoteDiagnostics::deviceName();
  HtmlBuffer &out = dashboardBuffer;
  if (!out.ensure()) return "";
  out.clear();
  out.append(R"HTML(<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><meta name="theme-color" content="#0f6b7a"><title>HP Print Server</title><style>*{box-sizing:border-box;margin:0;padding:0}body{font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif;background:#f2f4f8;color:#1f2937;line-height:1.5}#app{max-width:860px;margin:0 auto;padding:18px}header{display:flex;align-items:center;justify-content:space-between;padding:14px 0;border-bottom:1px solid #e5e7eb;margin-bottom:20px}h1{font-size:18px;font-weight:700;color:#111827;display:flex;align-items:center;gap:7px}h1 .dot{width:7px;height:7px;background:#10b981;border-radius:50%;display:inline-block}.sub{font-size:12px;color:#6b7280;margin-top:2px}.tabs{display:flex;gap:4px;overflow-x:auto;scrollbar-width:none;margin-bottom:18px}.tabs a{flex-shrink:0;padding:8px 14px;border-radius:8px;text-decoration:none;color:#6b7280;font-weight:600;font-size:13px;border:1px solid #e5e7eb;background:#fff}.tabs a.active{background:#0f6b7a;color:#fff;border-color:#0f6b7a}.tabs a:hover:not(.active){background:#f3f4f6}.cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:12px;margin-bottom:18px}card{background:#fff;border:1px solid #e5e7eb;border-radius:10px;padding:15px}card h3{font-size:11px;text-transform:uppercase;letter-spacing:.05em;color:#9ca3af;margin-bottom:8px;font-weight:700}.big{font-size:18px;font-weight:700;color:#111827}.ok{color:#10b981}.warn{color:#f59e0b}.muted{color:#6b7280;font-size:12.5px}section{margin-bottom:22px}section h2{font-size:14px;font-weight:700;margin-bottom:11px;color:#111827}.form{background:#fff;border:1px solid #e5e7eb;border-radius:10px;padding:15px}.field{margin-bottom:11px}.field label{display:block;font-size:12px;font-weight:600;color:#374151;margin-bottom:5px}.field input,.field select{width:100%;padding:9px 11px;border:1px solid #d1d5db;border-radius:8px;background:#fff;color:#111827;font-size:13px}.check{display:flex;align-items:center;gap:8px;margin-bottom:12px}.check input{width:auto}.btn{display:inline-flex;align-items:center;justify-content:center;padding:9px 16px;border-radius:8px;border:1px solid transparent;text-decoration:none;font-size:13px;font-weight:600;cursor:pointer}.btn.primary{background:#0f6b7a;color:#fff}.btn.primary:hover{background:#0d5a67}.btn.secondary{background:#fff;color:#374151;border-color:#d1d5db}.btn.secondary:hover{background:#f9fafb}.btn.small{padding:6px 12px;font-size:12px}.msg{padding:9px 12px;border-radius:8px;margin-bottom:11px;font-size:13px}.msg.muted{background:#f3f4f6;color:#374151;border:1px solid #e5e7eb}#refresh{font-size:11.5px;color:#6b7280}@media(max-width:640px){#app{padding:12px}}</style></head><body><div id="app"><header><h1><span class="dot"></span> HP Print Server</h1><a class="btn secondary small" href="/monitor" target="_blank">Live Monitor</a></header><nav class="tabs"><a class="active" href="#dash">Dashboard</a><a href="#net">Network</a><a href="#ble">Bluetooth</a><a href="#ota">OTA &amp; System</a></nav><div class="cards"><card><h3>Printer</h3><div class="big" id="cP">--</div><div class="muted" id="cPR">--</div></card><card><h3>Network</h3><div class="big" id="cN">--</div><div class="muted" id="cNI">--</div></card><card><h3>USB</h3><div class="big" id="cU">--</div><div class="muted" id="cUD">--</div></card><card><h3>BLE</h3><div class="big" id="cB">--</div><div class="muted" id="cBD">--</div></card></div>

<section id="dash"><h2>Status</h2><div class="form"><div class="field"><label>USB Printer</label><div class="muted" id="sPrinter">--</div></div><div class="field"><label>State</label><div class="muted" id="sState">--</div></div><div class="field"><label>RAW Client</label><div class="muted" id="sRaw">--</div></div><div class="field"><label>Wi-Fi</label><div class="muted" id="sWifi">--</div></div><div class="field"><label>IP Address</label><div class="muted" id="sIp">--</div></div><div class="field"><label>mDNS</label><div class="muted" id="sMdns">--</div></div><div class="field"><label>Free Heap</label><div class="muted" id="sHeap">--</div></div><div class="field"><label>PSRAM</label><div class="muted" id="sPsram">--</div></div></div></section>

<section id="net"><h2>Wi-Fi</h2><div class="form"><button class="btn secondary small" id="scanBtn" onclick="scanWifi()">Scan Networks</button><div id="scanMsg" class="muted" style="margin-top:8px"></div><select id="ssidSel"><option value="">Select a network&hellip;</option></select><form id="wifiForm" method="POST" action="/save"><div class="field"><label for="ssid">Network (SSID)</label><input id="ssid" name="ssid" value=")HTML"); out.appendEscaped(config.ssid); out.append(R"HTML(" maxlength="32"></div><div class="field"><label for="pass">Password</label><input id="pass" name="password" type="password" placeholder="Leave blank to keep current"></div><label class="check"><input type="checkbox" name="clearPassword" value="1"> Clear saved password</label><div class="msg muted">Saving reboots the device.</div><button class="btn primary" type="submit">Save &amp; Reboot</button></form></div></section>

<section id="ble"><h2>Bluetooth Console</h2><p class="msg muted">BLE GATT diagnostic console. Connect with an app like nRF Connect using the device name below.</p><div class="form"><form id="bleForm" method="POST" action="/ble/save"><label class="check"><input type="checkbox" name="bleEnabled" value="1" )HTML"); out.append(bleEnabled ? "checked" : ""); out.append(R"HTML(> Enable BLE</label><div class="field"><label for="bleName">Device Name</label><input id="bleName" name="bleName" value=")HTML"); out.appendEscaped(bleName); out.append(R"HTML(" maxlength="24" placeholder="HP-Print-Diagnostics"></div><div class="msg muted">Status: )HTML"); out.append(bleConnected ? "<span class='ok'>Connected</span>" : bleReady ? "<span class='ok'>Ready</span>" : "<span class='warn'>Disabled</span>"); out.append(R"HTML(</div><button class="btn primary" type="submit">Save Bluetooth</button></form></div></section>

<section id="ota"><h2>OTA &amp; System</h2><div class="form"><form id="otaForm" method="POST" action="/ota/save"><div class="field"><label for="otaPw">OTA Password</label><input id="otaPw" name="otaPassword" value=")HTML"); out.appendEscaped(config.otaPassword); out.append(R"HTML(" maxlength="32" placeholder="Password"></div><div class="msg muted">Password for wireless firmware uploads. Change before deploying.</div><button class="btn primary" type="submit">Save OTA Password</button></form><div style="margin-top:16px"><button class="btn secondary small" onclick="if(confirm('Reboot now?')){fetch('/save',{method:'POST',body:'ssid='+encodeURIComponent(')HTML); out.appendEscaped(config.ssid); out.append(R"HTML('+'&password='+encodeURIComponent(')HTML); out.appendEscaped(config.password); out.append(R"HTML('+'&reboot=1')}).then(()=>alert('Rebooting&hellip;'))">Reboot Device</button></div></div></section>

<script>const $=id=>document.getElementById(id);function esc(t){const d=document.createElement('div');d.textContent=t;return d.innerHTML}async function scanWifi(){const b=$('scanBtn'),m=$('scanMsg');b.disabled=true;m.textContent='Scanning...';try{const r=await fetch('/scan.json?ts='+Date.now());const a=await r.json();const sel=$('ssidSel');sel.innerHTML='<option value="">Select a network&hellip;</option>';a.sort((x,y)=>y.rssi-x.rssi);a.forEach(x=>{const o=document.createElement('option');o.value=esc(x.ssid);o.textContent=esc(x.ssid)+' ('+x.rssi+' dBm)';sel.appendChild(o);});m.textContent='Found '+a.length+'.';}catch(e){m.textContent='Scan failed.';}finally{b.disabled=false;}}async function refresh(){try{const r=await fetch('/status.json?ts='+Date.now());const s=await r.json();$('cP').textContent=s.usbAttached?s.printerName:'No USB Printer';$('cPR').textContent=s.printerReason;$('cP').className='big '+(s.usbAttached?'ok':'muted');$('cN').textContent=s.wifi;$('cNI').textContent=s.ip||'&ndash;';$('cU').textContent=s.usbStatus;$('cUD').textContent=s.usbWedged?'Wedged':'OK';$('cUD').className='muted '+(s.usbWedged?'warn':'');$('cB').textContent=s.bleEnabled?(s.bleConnected?'Connected':'Advertising')+' &bull; '+s.bleName:'Disabled';$('sPrinter').textContent=s.usbAttached?s.printerName:'Not attached';$('sState').textContent=s.printerState;$('sRaw').textContent=s.rawConnected?'Connected':'Idle';$('sWifi').textContent=s.wifi;$('sIp').textContent=s.ip||'&ndash;';$('sMdns').textContent=s.mdnsReady?'printer.local':'&ndash;';$('sHeap').textContent=(s.freeHeap/1024).toFixed(0)+' KB';$('sPsram').textContent=(s.psramFree/1024).toFixed(0)+' / '+(s.psramTotal/1024).toFixed(0)+' KB';$('#refresh').textContent='Updated '+new Date().toLocaleTimeString();}catch(e){}}setInterval(refresh,5000);refresh();</script></div></body></html>)HTML");
  return out.c_str();
}

void handleRoot() { const char *page = dashboard(); const size_t length = strlen(page); configServer.setContentLength(length); configServer.send(200, "text/html; charset=utf-8"); if (length) configServer.sendContent(page, length); }
void handleMonitor() {
  size_t length = 0; const char *ring = RemoteDiagnostics::logData(length);
  configServer.setContentLength(length);
  configServer.send(200, "text/plain; charset=utf-8");
  if (length) configServer.sendContent(ring, length);
}

void handleSave() { if (!configServer.hasArg("ssid") || configServer.arg("ssid").isEmpty()) { configServer.send(400, "text/plain; charset=utf-8", "Wi-Fi network name is required."); return; } config.ssid = configServer.arg("ssid"); if (configServer.hasArg("clearPassword")) config.password = ""; else if (configServer.hasArg("password") && !configServer.arg("password").isEmpty()) config.password = configServer.arg("password"); if (!saveConfig()) { configServer.send(500, "text/plain; charset=utf-8", "Could not save Wi-Fi settings. The device was not restarted."); return; } if (configServer.hasArg("reboot")) { configServer.send(200, "text/html; charset=utf-8", "<p>Rebooting…</p>"); delay(200); ESP.restart(); return; } configServer.send(200, "text/html; charset=utf-8", "<p>Wi-Fi settings saved. Rebooting…</p>"); delay(300); ESP.restart(); }

void handleSaveBle() {
  config.bleEnabled = configServer.hasArg("bleEnabled");
  String name = configServer.arg("bleName"); name.trim();
  if (name.isEmpty()) name = "HP-Print-Diagnostics";
  if (name.length() > 24) name = name.substring(0, 24);
  config.bleName = name;
  if (!saveConfig()) { configServer.send(500, "text/plain; charset=utf-8", "Could not save Bluetooth settings."); return; }
  RemoteDiagnostics::configure(config.bleEnabled, config.bleName);
  configServer.send(200, "text/html; charset=utf-8", "<p>Bluetooth settings applied.</p><p><a href='/'>Back to dashboard</a></p>");
}

void handleSaveOta() {
  String pw = configServer.arg("otaPassword"); pw.trim();
  if (pw.length() < 1 || pw.length() > 32) { configServer.send(400, "text/plain; charset=utf-8", "OTA password must be 1-32 characters."); return; }
  config.otaPassword = pw;
  if (!saveConfig()) { configServer.send(500, "text/plain; charset=utf-8", "Could not save OTA password."); return; }
  ArduinoOTA.setPassword(config.otaPassword);
  configServer.send(200, "text/html; charset=utf-8", "<p>OTA password changed.</p><p><a href='/'>Back to dashboard</a></p>");
}

void serviceMonitor() {
  if (monitorServer.hasClient()) {
    WiFiClient c = monitorServer.available();
    if (c) {
      if (monitorClient) monitorClient.stop();
      monitorClient = c;
      monitorClient.setNoDelay(true);
      size_t length = 0; const char *ring = RemoteDiagnostics::logData(length);
      monitorClient.write(reinterpret_cast<const uint8_t *>(ring), length);
      monitorSequence = RemoteDiagnostics::logSequence();
      monitorClient.println();
      monitorClient.println("[wireless-serial] connected on TCP 23");
    }
  }
  if (!monitorClient || !monitorClient.connected()) {
    if (monitorClient) monitorClient.stop();
    monitorClient = WiFiClient();
    return;
  }
  uint32_t seq = RemoteDiagnostics::logSequence();
  if (seq == monitorSequence) return;
  size_t length = 0; const char *ring = RemoteDiagnostics::logData(length);
  size_t newBytes = seq - monitorSequence;
  if (newBytes > length) newBytes = length;
  if (newBytes) {
    monitorClient.write(reinterpret_cast<const uint8_t *>(ring + length - newBytes), newBytes);
  }
  monitorSequence = seq;
}

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
  Log.println("[WDT] Task watchdog armed (60 s)");
}

void validateOtaImage() {
  if (otaValidated || millis() < 10000UL) return;
  otaValidated = true;
  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  if (running && esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
      Log.println("[OTA] Pending image marked valid; rollback cancelled");
    }
  }
}

void handleLog() { size_t length = 0; const char *ring = RemoteDiagnostics::logData(length); if (configServer.hasArg("tail")) { const long tail = configServer.arg("tail").toInt(); if (tail > 0 && (size_t)tail < length) { ring += length - (size_t)tail; length = (size_t)tail; } } configServer.setContentLength(length); configServer.send(200, "text/plain; charset=utf-8"); if (length) configServer.sendContent(ring, length); }

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
    Log.println("[AP] Station link up; configuration AP stopped");
  }
  startRawDiscovery();
  usbPrinterBackend.restartRawListener();
  ippService.rebind();
  if (!otaReady) startOtaIfPossible();
  Log.println("[WiFi] Station link ready; RAW/IPP listeners re-bound");
}

void serviceWifi() {
  const int status = (int)WiFi.status();
  if (status == WL_CONNECTED && lastWifiStatus != WL_CONNECTED) {
    onWifiConnected();
  } else if (status != WL_CONNECTED && lastWifiStatus == WL_CONNECTED) {
    Log.println("[WiFi] Station link lost; retrying in background");
  }

  if (status != WL_CONNECTED && !config.ssid.isEmpty() && millis() - lastWifiAttemptMs >= 30000UL) {
    lastWifiAttemptMs = millis();
    Log.printf("[WiFi] Retrying saved network %s\n", config.ssid.c_str());
    WiFi.reconnect();
  }
  lastWifiStatus = status;
}

void setup() {
  Log.begin(115200); delay(500); Log.println(); SystemDiagnostics::logBoot(); Log.println("=== ESP32-S3 HP PCL3GUI Print Server ===");
  loadBootCount();
  startWatchdog();
  RemoteDiagnostics::begin();
  RemoteDiagnostics::setCommandHook(diagCommand);
  Log.printf("[MEM] internal-heap-free=%lu psram-total=%u psram-free=%u\n",
                (unsigned long)ESP.getFreeHeap(), (unsigned)ESP.getPsramSize(),
                (unsigned)ESP.getFreePsram());
  Log.println("[MODE] RAW 9100 + IPP 631 PCL3GUI; both print through the classic USB Printer Class interface");
  Log.println("[MODE] IPP-over-USB printing is disabled/not used; scanner USB backend is disabled");
  loadConfig(); RemoteDiagnostics::configure(config.bleEnabled, config.bleName); if (!connectWiFi()) startConfigAP(); startRawDiscovery(); startOtaIfPossible();
  lastWifiStatus = (int)WiFi.status();
  usbPrinterBackend.begin(); ippService.begin();
  configServer.on("/", HTTP_GET, handleRoot); configServer.on("/scan.json", HTTP_GET, sendJsonScan); configServer.on("/status.json", HTTP_GET, sendJsonStatus); configServer.on("/log", HTTP_GET, handleLog); configServer.on("/monitor", HTTP_GET, handleMonitor); configServer.on("/save", HTTP_POST, handleSave); configServer.on("/ble/save", HTTP_POST, handleSaveBle); configServer.on("/ota/save", HTTP_POST, handleSaveOta); ensureUsbScannerWebRoutesInstalled(); configServer.begin(); monitorServer.begin();
  Log.println("[HTTP] Configuration server ready"); Log.printf("[HTTP] Open http://%s\n", mdnsReady ? "printer.local" : (activeIp().length() ? activeIp().c_str() : "device-address")); Log.println("[RAW] TCP 9100 server enabled"); Log.println("[IPP] TCP 631 service enabled; document-format-supported=application/vnd.hp-pcl; version=PCL3GUI");
}

void loop() {
  esp_task_wdt_reset();
  serviceWifi();
  serviceMdns();
  configServer.handleClient();
  serviceMonitor();
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
    if (changed) { Log.printf("[STATUS] WiFi=%d IP=%s USB=%d printer=%s raw=%s%s\n", wifiState, ip.c_str(), usbState, printerState.c_str(), rawConnected ? "connected" : "idle", usbStatusValid ? (String(" usbport=0x") + String(usbPort, HEX)).c_str() : " usbstatus=unavailable"); RemoteDiagnostics::publishf("[STATUS] WiFi=%d IP=%s USB=%d printer=%s raw=%s\n", wifiState, ip.c_str(), usbState, printerState.c_str(), rawConnected ? "connected" : "idle"); lastWifiState = wifiState; lastIp = ip; lastUsbState = usbState; lastPrinterState = printerState; lastRawConnected = rawConnected; lastUsbStatusValid = usbStatusValid; lastUsbPort = usbPort; initialized = true; }
  }
}
