#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include "printer_protocols.h"

// -----------------------------------------------------------------------------
// HP Print Server - Arduino IDE foundation
// -----------------------------------------------------------------------------
// This firmware deliberately implements only the pieces that are actually
// supported today. IPP/WSD/RAW/LPR/USB are represented by the architecture and
// configuration model, but are NOT advertised as working print services until
// their implementations exist.

WebServer server(80);
Preferences prefs;

static const char *CONFIG_NAMESPACE = "hp-print";
static const char *CONFIG_AP_SSID = "HP-Print-Server";
static const char *CONFIG_AP_PASSWORD = "configureme"; // TODO: make configurable
static const char *MDNS_HOSTNAME = "hp-print-server";

struct Config {
  String wifiSsid;
  String wifiPassword;
  String printerName;
  String printerModel;
  String printerIp;
  String printerHost;
  String lprQueue;
  uint16_t printerPort;
  PrinterTransport transport;
  PrinterDiscovery discovery;
};

Config config;

// -----------------------------------------------------------------------------
// Configuration persistence
// -----------------------------------------------------------------------------

void setDefaults() {
  config.wifiSsid = "";
  config.wifiPassword = "";
  config.printerName = "HP Print Server";
  config.printerModel = "";
  config.printerIp = "";
  config.printerHost = "";
  config.lprQueue = "lp";
  config.printerPort = 9100;
  config.transport = TRANSPORT_JETDIRECT_RAW;
  config.discovery = DISCOVERY_MANUAL_IP;
}

void loadConfig() {
  setDefaults();

  if (!prefs.begin(CONFIG_NAMESPACE, true)) {
    Serial.println("[Config] Preferences open failed; using defaults");
    return;
  }

  config.wifiSsid = prefs.getString("ssid", config.wifiSsid);
  config.wifiPassword = prefs.getString("pass", config.wifiPassword);
  config.printerName = prefs.getString("name", config.printerName);
  config.printerModel = prefs.getString("model", config.printerModel);
  config.printerIp = prefs.getString("ip", config.printerIp);
  config.printerHost = prefs.getString("host", config.printerHost);
  config.lprQueue = prefs.getString("lprq", config.lprQueue);
  config.printerPort = prefs.getUShort("port", config.printerPort);

  uint8_t transport = prefs.getUChar("transport", TRANSPORT_JETDIRECT_RAW);
  uint8_t discovery = prefs.getUChar("discovery", DISCOVERY_MANUAL_IP);
  config.transport = isValidTransport(transport)
                       ? (PrinterTransport)transport
                       : TRANSPORT_JETDIRECT_RAW;
  config.discovery = isValidDiscovery(discovery)
                       ? (PrinterDiscovery)discovery
                       : DISCOVERY_MANUAL_IP;

  prefs.end();
}

bool saveConfig() {
  if (!prefs.begin(CONFIG_NAMESPACE, false)) {
    Serial.println("[Config] Preferences open failed");
    return false;
  }

  bool ok = true;
  ok &= prefs.putString("ssid", config.wifiSsid) > 0 || config.wifiSsid.isEmpty();
  ok &= prefs.putString("pass", config.wifiPassword) > 0 || config.wifiPassword.isEmpty();
  ok &= prefs.putString("name", config.printerName) > 0;
  ok &= prefs.putString("model", config.printerModel) > 0 || config.printerModel.isEmpty();
  ok &= prefs.putString("ip", config.printerIp) > 0 || config.printerIp.isEmpty();
  ok &= prefs.putString("host", config.printerHost) > 0 || config.printerHost.isEmpty();
  ok &= prefs.putString("lprq", config.lprQueue) > 0;
  ok &= prefs.putUShort("port", config.printerPort) > 0;
  ok &= prefs.putUChar("transport", (uint8_t)config.transport) > 0;
  ok &= prefs.putUChar("discovery", (uint8_t)config.discovery) > 0;
  prefs.end();

  Serial.println(ok ? "[Config] Saved" : "[Config] Save failed");
  return ok;
}

// -----------------------------------------------------------------------------
// Input validation
// -----------------------------------------------------------------------------

bool validPort(const String &value, uint16_t &port) {
  if (value.isEmpty()) return false;
  long parsed = value.toInt();
  if (parsed < 1 || parsed > 65535) return false;
  port = (uint16_t)parsed;
  return true;
}

void applyWebConfig() {
  if (server.hasArg("ssid")) config.wifiSsid = server.arg("ssid");

  // An empty password means "keep the existing password". This prevents the
  // HTML page from accidentally erasing stored credentials.
  if (server.hasArg("password") && !server.arg("password").isEmpty()) {
    config.wifiPassword = server.arg("password");
  }

  if (server.hasArg("printerName")) config.printerName = server.arg("printerName");
  if (server.hasArg("printerModel")) config.printerModel = server.arg("printerModel");
  if (server.hasArg("printerIp")) config.printerIp = server.arg("printerIp");
  if (server.hasArg("printerHost")) config.printerHost = server.arg("printerHost");
  if (server.hasArg("lprQueue")) config.lprQueue = server.arg("lprQueue");

  uint16_t port;
  if (server.hasArg("printerPort") && validPort(server.arg("printerPort"), port)) {
    config.printerPort = port;
  }

  if (server.hasArg("transport")) {
    int value = server.arg("transport").toInt();
    if (isValidTransport((uint8_t)value)) config.transport = (PrinterTransport)value;
  }

  if (server.hasArg("discovery")) {
    int value = server.arg("discovery").toInt();
    if (isValidDiscovery((uint8_t)value)) config.discovery = (PrinterDiscovery)value;
  }
}

// -----------------------------------------------------------------------------
// Web UI helpers
// -----------------------------------------------------------------------------

String htmlEscape(const String &value) {
  String out = value;
  out.replace("&", "&amp;");
  out.replace("<", "&lt;");
  out.replace(">", "&gt;");
  out.replace("\"", "&quot;");
  out.replace("'", "&#39;");
  return out;
}

String wifiStatusText() {
  if (WiFi.status() == WL_CONNECTED) {
    return "Connected to <b>" + htmlEscape(config.wifiSsid) + "</b> — " + WiFi.localIP().toString();
  }
  return "Not connected — configuration AP is available at " + WiFi.softAPIP().toString();
}

String transportOptions() {
  String result;
  for (uint8_t i = PRINTER_TRANSPORT_FIRST; i <= PRINTER_TRANSPORT_LAST; ++i) {
    PrinterTransport t = (PrinterTransport)i;
    result += "<option value='" + String(i) + "'";
    if (config.transport == t) result += " selected";
    result += ">" + htmlEscape(transportName(t)) + "</option>";
  }
  return result;
}

String discoveryOptions() {
  String result;
  for (uint8_t i = PRINTER_DISCOVERY_FIRST; i <= PRINTER_DISCOVERY_LAST; ++i) {
    PrinterDiscovery d = (PrinterDiscovery)i;
    result += "<option value='" + String(i) + "'";
    if (config.discovery == d) result += " selected";
    result += ">" + htmlEscape(discoveryName(d)) + "</option>";
  }
  return result;
}

String page() {
  String html = R"rawliteral(<!doctype html><html><head>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>HP Print Server</title>
<style>
body{font-family:system-ui,Arial,sans-serif;max-width:850px;margin:24px auto;padding:0 16px;background:#f4f4f4;color:#222}
section{background:#fff;padding:20px;margin:16px 0;border-radius:12px;box-shadow:0 2px 8px #0001}
input,select{width:100%;box-sizing:border-box;padding:10px;margin:6px 0 14px;border:1px solid #aaa;border-radius:6px}
button{padding:11px 18px;border:0;border-radius:7px;cursor:pointer}.primary{background:#222;color:#fff}
.status{padding:12px;background:#eee;border-radius:7px}.grid{display:grid;grid-template-columns:1fr 1fr;gap:14px}
.note{color:#555;font-size:.9rem}.tag{font-family:monospace;background:#eee;padding:2px 5px;border-radius:4px}
@media(max-width:650px){.grid{grid-template-columns:1fr}}
</style></head><body>
<h1>HP Print Server</h1>
<section><h2>Network</h2>
<div class='status'>)rawliteral" + wifiStatusText() + R"rawliteral(</div>
<form method='POST' action='/save'>
<label>Wi-Fi SSID</label><input name='ssid' value=')rawliteral" + htmlEscape(config.wifiSsid) + R"rawliteral(' maxlength='32'>
<label>Wi-Fi password</label><input type='password' name='password' placeholder='Leave blank to keep the saved password'>
<button class='primary'>Save Wi-Fi settings</button></form>
<form method='GET' action='/scan'><button type='submit'>Scan nearby Wi-Fi networks</button></form>
</section>

<section><h2>Printer</h2>
<form method='POST' action='/save'>
<div class='grid'><div><label>Printer name</label><input name='printerName' value=')rawliteral" + htmlEscape(config.printerName) + R"rawliteral('></div>
<div><label>Printer model</label><input name='printerModel' value=')rawliteral" + htmlEscape(config.printerModel) + R"rawliteral(' placeholder='Example: HP Smart Tank 520'></div></div>
<label>Discovery method</label><select name='discovery'>)rawliteral" + discoveryOptions() + R"rawliteral(</select>
<label>Print transport</label><select name='transport'>)rawliteral" + transportOptions() + R"rawliteral(</select>
<label>Printer IP</label><input name='printerIp' value=')rawliteral" + htmlEscape(config.printerIp) + R"rawliteral(' placeholder='Optional when discovery is used'>
<label>Printer hostname</label><input name='printerHost' value=')rawliteral" + htmlEscape(config.printerHost) + R"rawliteral(' placeholder='Example: printer.local'>
<div class='grid'><div><label>TCP port</label><input type='number' name='printerPort' value=')rawliteral" + String(config.printerPort) + R"rawliteral(' min='1' max='65535'></div>
<div><label>LPR queue</label><input name='lprQueue' value=')rawliteral" + htmlEscape(config.lprQueue) + R"rawliteral('></div></div>
<button class='primary'>Save printer settings</button></form>
</section>

<section><h2>Implementation status</h2>
<p><span class='tag'>Implemented</span> Wi-Fi station + persistent configuration + configuration AP + web UI + mDNS hostname.</p>
<p><span class='tag'>Planned</span> mDNS printer discovery, WS-Discovery, SSDP, SNMP, IPP, IPPS, RAW 9100, LPR, WSD and USB Host print pipelines.</p>
<p class='note'>The firmware will not advertise a print protocol until that protocol has a real implementation. This prevents Android and HP software from discovering a service that cannot actually accept jobs.</p>
</section>
</body></html>)rawliteral";
  return html;
}

// -----------------------------------------------------------------------------
// Wi-Fi
// -----------------------------------------------------------------------------

void startConfigurationAP() {
  WiFi.mode(WIFI_AP_STA);
  bool ok = WiFi.softAP(CONFIG_AP_SSID, CONFIG_AP_PASSWORD, 1, false, 4);
  if (!ok) {
    Serial.println("[AP] Failed to start configuration AP");
    return;
  }

  Serial.print("[AP] SSID: ");
  Serial.println(CONFIG_AP_SSID);
  Serial.print("[AP] IP: http://");
  Serial.println(WiFi.softAPIP());
}

bool connectConfiguredWiFi() {
  if (config.wifiSsid.isEmpty()) {
    Serial.println("[WiFi] No saved SSID; configuration required");
    return false;
  }

  WiFi.disconnect(false, true);
  delay(100);
  WiFi.begin(config.wifiSsid.c_str(), config.wifiPassword.c_str());

  Serial.printf("[WiFi] Connecting to %s", config.wifiSsid.c_str());
  unsigned long started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 20000UL) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("[WiFi] Failed, status=%d\n", WiFi.status());
    return false;
  }

  Serial.print("[WiFi] IP: ");
  Serial.println(WiFi.localIP());
  return true;
}

void startMDNS() {
  if (WiFi.status() != WL_CONNECTED) return;

  MDNS.end();
  if (!MDNS.begin(MDNS_HOSTNAME)) {
    Serial.println("[mDNS] Failed to start");
    return;
  }

  // Only advertise the device hostname for now. No fake _ipp/_ipps/_printer
  // services are published until their corresponding servers exist.
  Serial.print("[mDNS] Host: ");
  Serial.print(MDNS_HOSTNAME);
  Serial.println(".local");
}

// -----------------------------------------------------------------------------
// HTTP handlers
// -----------------------------------------------------------------------------

void handleRoot() {
  server.send(200, "text/html; charset=utf-8", page());
}

void handleSave() {
  applyWebConfig();

  if (config.printerPort == 0) {
    server.send(400, "text/plain", "Invalid printer port\n");
    return;
  }

  if (!saveConfig()) {
    server.send(500, "text/plain", "Failed to save configuration\n");
    return;
  }

  // Do not block the browser request with another 20-second connection attempt.
  // Rebooting after a successful save gives the network stack a clean state.
  server.send(200, "text/html; charset=utf-8",
              "<html><body><h2>Configuration saved</h2>"
              "<p>The ESP32-S3 will restart and apply the settings.</p>"
              "</body></html>");
  delay(300);
  ESP.restart();
}

void handleScan() {
  int count = WiFi.scanNetworks(false, true);
  String html = "<html><head><meta name='viewport' content='width=device-width,initial-scale=1'><title>Wi-Fi scan</title></head><body><h1>Nearby Wi-Fi</h1>";

  if (count <= 0) {
    html += "<p>No networks found.</p>";
  } else {
    html += "<form method='GET' action='/'><select name='ssid'>";
    for (int i = 0; i < count; ++i) {
      String ssid = WiFi.SSID(i);
      if (ssid.isEmpty()) ssid = "(hidden)";
      html += "<option>" + htmlEscape(ssid) + " — " + String(WiFi.RSSI(i)) + " dBm</option>";
    }
    html += "</select></form><ul>";
    for (int i = 0; i < count; ++i) {
      String ssid = WiFi.SSID(i);
      if (ssid.isEmpty()) ssid = "(hidden)";
      html += "<li>" + htmlEscape(ssid) + " — " + String(WiFi.RSSI(i)) + " dBm — channel " + String(WiFi.channel(i)) + "</li>";
    }
    html += "</ul>";
  }

  WiFi.scanDelete();
  html += "<p><a href='/'>Back</a></p></body></html>";
  server.send(200, "text/html; charset=utf-8", html);
}

void handleHealth() {
  String body = WiFi.status() == WL_CONNECTED ? "OK\n" : "CONFIGURATION_REQUIRED\n";
  body += "wifi=" + String(WiFi.status()) + "\n";
  body += "transport=" + String(transportName(config.transport)) + "\n";
  body += "discovery=" + String(discoveryName(config.discovery)) + "\n";
  server.send(200, "text/plain; charset=utf-8", body);
}

void handleNotFound() {
  server.send(404, "text/plain; charset=utf-8", "Not found\n");
}

// -----------------------------------------------------------------------------
// Arduino entry points
// -----------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("========================================");
  Serial.println("       HP Print Server / ESP32-S3");
  Serial.println("========================================");
  Serial.printf("[SYS] Free heap: %u bytes\n", ESP.getFreeHeap());

  loadConfig();
  startConfigurationAP();
  connectConfiguredWiFi();
  startMDNS();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/scan", HTTP_GET, handleScan);
  server.on("/health", HTTP_GET, handleHealth);
  server.on("/save", HTTP_POST, handleSave);
  server.onNotFound(handleNotFound);
  server.begin();

  Serial.println("[HTTP] Configuration server ready");
  Serial.println("[HTTP] Connect to the configuration AP if Wi-Fi is not configured");
}

void loop() {
  server.handleClient();
  delay(2);
}
