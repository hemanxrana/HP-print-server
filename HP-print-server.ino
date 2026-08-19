#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include "usb_printer_backend.h"

// ESP32-S3 USB-to-Wi-Fi RAW print server.
// Network side: JetDirect/AppSocket on TCP 9100 only.
// Print data is forwarded byte-for-byte to the selected USB Printer Class
// Bulk OUT endpoint. No IPP, document conversion, spool or print-language
// emulation is performed here.

static constexpr const char *RAW_HOSTNAME = "hp-print-server";
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
  bool usbAuto = true;
  uint8_t usbInterface = 0;
  uint8_t usbAlt = 0;
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
  config.usbAuto = true;
  config.usbInterface = 0;
  config.usbAlt = 0;
}

void loadConfig() {
  defaults();
  if (!preferences.begin(CONFIG_NS, true)) {
    Serial.println("[CFG] Preferences read failed; using defaults");
    return;
  }
  config.ssid = preferences.getString("ssid", config.ssid);
  config.password = preferences.getString("pass", config.password);
  config.usbAuto = preferences.getBool("usbauto", config.usbAuto);
  config.usbInterface = preferences.getUChar("usbif", config.usbInterface);
  config.usbAlt = preferences.getUChar("usbalt", config.usbAlt);
  preferences.end();
}

bool saveConfig() {
  if (!preferences.begin(CONFIG_NS, false)) return false;
  bool ok = true;
  ok &= preferences.putString("ssid", config.ssid) > 0 || config.ssid.isEmpty();
  ok &= preferences.putString("pass", config.password) > 0 || config.password.isEmpty();
  ok &= preferences.putBool("usbauto", config.usbAuto);
  ok &= preferences.putUChar("usbif", config.usbInterface) > 0;
  ok &= preferences.putUChar("usbalt", config.usbAlt) > 0;
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
    Serial.println("[mDNS] Failed to start raw-print discovery responder");
    return;
  }
  MDNS.setInstanceName("HP Print Server RAW 9100");
  if (MDNS.addService("pdl-datastream", "tcp", 9100)) {
    MDNS.addServiceTxt("pdl-datastream", "tcp", "txtvers", "1");
    MDNS.addServiceTxt("pdl-datastream", "tcp", "note", "RAW 9100");
    Serial.println("[mDNS] RAW 9100 discovery advertised");
  }
}

String usbInterfaceLabel(const UsbPrinterInterfaceInfo &p, bool active) {
  String s = String("IF ") + String(p.interfaceNumber) + " / ALT " + String(p.alternateSetting) + " / protocol 0x";
  if (p.protocol < 16) s += "0";
  s += String(p.protocol, HEX);
  s += " / OUT 0x";
  if (p.bulkOut.address < 16) s += "0";
  s += String(p.bulkOut.address, HEX);
  s += " / IN ";
  if (p.bulkIn.valid()) {
    s += "0x";
    if (p.bulkIn.address < 16) s += "0";
    s += String(p.bulkIn.address, HEX);
  } else {
    s += "none";
  }
  if (active) s += " [ACTIVE]";
  return s;
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

String wifiOptionsHtml() {
  String html;
  html.reserve(2500);
  const int n = WiFi.scanNetworks(false, true);
  for (int i = 0; i < n; ++i) {
    String ssid = WiFi.SSID(i);
    if (ssid.isEmpty()) continue;
    html += String("<option value='") + esc(ssid) + "'></option>";
  }
  WiFi.scanDelete();
  return html;
}

String dashboard() {
  String wifi;
  if (WiFi.status() == WL_CONNECTED) {
    wifi = String("Connected — ") + WiFi.localIP().toString();
  } else if (WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA) {
    wifi = String("Configuration AP — ") + WiFi.softAPIP().toString();
  } else {
    wifi = "Not connected";
  }

  String selected = usbHost.selectedInterface()
      ? usbInterfaceLabel(*usbHost.selectedInterface(), true)
      : "none";

  String usbOptions;
  if (!usbHost.device().attached || usbHost.interfaceCount() == 0) {
    usbOptions = "<p>No USB printer interfaces detected.</p>";
  } else {
    usbOptions = "<form method='POST' action='/usb'><label><input type='radio' name='mode' value='auto' ";
    usbOptions += config.usbAuto ? "checked" : "";
    usbOptions += "> Automatic</label><br>";
    for (uint8_t i = 0; i < usbHost.interfaceCount(); ++i) {
      const UsbPrinterInterfaceInfo *p = usbHost.interfaceAt(i);
      if (!p) continue;
      const bool checked = !config.usbAuto && p->interfaceNumber == config.usbInterface && p->alternateSetting == config.usbAlt;
      const bool active = usbHost.selectedInterface() == p;
      usbOptions += String("<label><input type='radio' name='mode' value='manual:") + String(p->interfaceNumber) + ":" + String(p->alternateSetting) + "' ";
      usbOptions += checked ? "checked" : "";
      usbOptions += String("> ") + esc(usbInterfaceLabel(*p, active)) + "</label><br>";
    }
    usbOptions += "<br><button type='submit'>Apply USB interface</button></form>";
  }

  String html;
  html.reserve(8500);
  html += "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'><title>HP Print Server</title>";
  html += "<style>body{font-family:system-ui,Arial;max-width:820px;margin:24px auto;padding:0 16px;background:#f5f5f5;color:#222}section{background:#fff;padding:20px;margin:16px 0;border-radius:12px;box-shadow:0 2px 8px #0001}input{box-sizing:border-box;padding:10px;margin:6px 0 14px;border:1px solid #aaa;border-radius:7px;width:100%}input[type=radio]{width:auto;margin-right:8px}button{padding:10px 14px;border:0;border-radius:7px;background:#222;color:#fff}.ssidRow{display:flex;gap:8px;align-items:center}.ssidRow input{flex:1}.status{padding:12px;background:#eee;border-radius:7px}code{word-break:break-all}.hint{font-size:.9em;color:#666}</style></head><body>";
  html += "<h1>HP Print Server</h1>";
  html += String("<section><h2>Status</h2><div class='status'>Wi-Fi: ") + esc(wifi) + "<br>USB host: " + esc(usbStateText()) + "<br>Printer: " + esc(printerStateText()) + "<br>RAW JetDirect/AppSocket: TCP <b>9100</b></div></section>";
  html += String("<section><h2>Wi-Fi</h2><form method='POST' action='/save'><label>SSID</label><div class='ssidRow'><input id='ssid' name='ssid' list='wifiList' value='") + esc(config.ssid) + "' maxlength='32' autocomplete='off'><button type='button' onclick='scanWifi()'>Search</button></div><datalist id='wifiList'>" + wifiOptionsHtml() + "</datalist><div class='hint'>Type to search, or press Search to scan nearby networks.</div><label>Password</label><input type='password' name='password' placeholder='Leave blank to keep current password'><button type='submit'>Save &amp; restart</button></form></section>";
  html += String("<section><h2>USB printer interface</h2><p>Device: ") + (usbHost.device().attached ? String("VID 0x") + String(usbHost.device().vid, HEX) + " / PID 0x" + String(usbHost.device().pid, HEX) : String("none"));
  html += String("</p><p>Active: <b>") + esc(selected) + "</b></p>" + usbOptions + "</section>";
  html += String("<section><h2>RAW printing</h2><p>Connect a client directly to <code>") + String(RAW_HOSTNAME) + ":9100</code> or <code>" + (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("192.168.4.1")) + ":9100</code>.</p><p>The server does not add Content-Length, IPP headers, PJL, form feeds, or any other print data. The incoming byte stream is passed unchanged to USB.</p><p>Use a print stream the HP printer itself understands (for example a valid PCL/PJL stream). PDF/PNG/PWG/URF conversion is not performed.</p></section>";
  html += "<script>async function scanWifi(){const b=document.querySelector('.ssidRow button');b.disabled=true;b.textContent='Searching…';try{const r=await fetch('/scan.json');const a=await r.json();const d=document.getElementById('wifiList');d.innerHTML='';a.forEach(x=>{const o=document.createElement('option');o.value=x.ssid;d.appendChild(o)});}catch(e){alert('Wi-Fi scan failed');}finally{b.disabled=false;b.textContent='Search';}}</script></body></html>";
  return html;
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
    out += String("{\"ssid\":\"") + jsonEsc(ssid) + "\",\"rssi\":" + String(WiFi.RSSI(i)) + "}";
  }
  out += "]";
  WiFi.scanDelete();
  configServer.send(200, "application/json", out);
}

void handleRoot() {
  configServer.send(200, "text/html; charset=utf-8", dashboard());
}

void handleSave() {
  if (configServer.hasArg("ssid")) config.ssid = configServer.arg("ssid");
  if (configServer.hasArg("password") && !configServer.arg("password").isEmpty()) config.password = configServer.arg("password");
  saveConfig();
  configServer.send(200, "text/html; charset=utf-8", "<p>Saved. Rebooting…</p>");
  delay(300);
  ESP.restart();
}

void handleUsb() {
  if (configServer.hasArg("mode")) {
    const String m = configServer.arg("mode");
    if (m == "auto") {
      config.usbAuto = true;
    } else if (m.startsWith("manual:")) {
      const int a = m.indexOf(':', 7);
      if (a > 7) {
        config.usbAuto = false;
        config.usbInterface = (uint8_t)m.substring(7, a).toInt();
        config.usbAlt = (uint8_t)m.substring(a + 1).toInt();
      }
    }
    saveConfig();
    usbHost.setInterfaceSelection(config.usbAuto, config.usbInterface, config.usbAlt);
  }
  configServer.sendHeader("Location", "/");
  configServer.send(303, "text/plain", "Applied");
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("=== ESP32-S3 RAW 9100 USB Print Server ===");
  Serial.println("[MODE] JetDirect/AppSocket only; IPP disabled");

  loadConfig();
  if (!connectWiFi()) startConfigAP();

  startRawDiscovery();

  // Apply the saved interface choice before enumeration starts.
  usbHost.setInterfaceSelection(config.usbAuto, config.usbInterface, config.usbAlt);
  usbPrinterBackend.begin();

  configServer.on("/", HTTP_GET, handleRoot);
  configServer.on("/scan.json", HTTP_GET, sendJsonScan);
  configServer.on("/save", HTTP_POST, handleSave);
  configServer.on("/usb", HTTP_POST, handleUsb);
  configServer.begin();

  Serial.println("[HTTP] Configuration server ready");
  Serial.print("[HTTP] Open http://");
  Serial.println(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : WiFi.softAPIP().toString());
  Serial.println("[RAW] TCP 9100 server enabled");
}

void loop() {
  configServer.handleClient();
  usbHost.poll();
  usbPrinterBackend.poll();

  if (millis() - lastStatus > 5000) {
    lastStatus = millis();
    Serial.printf("[STATUS] WiFi=%d IP=%s USB=%d printer=%s\n",
                  (int)WiFi.status(),
                  WiFi.localIP().toString().c_str(),
                  (int)usbHost.state(),
                  printerStateText().c_str());
  }
}
