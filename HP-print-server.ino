#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>

WebServer server(80);
Preferences prefs;

struct Config {
  String wifiSsid;
  String wifiPassword;
  String printerName;
  String printerModel;
  String printerIp;
  uint16_t printerPort;
};

Config config;

const char *AP_SSID = "HP-Print-Server";
const char *AP_PASSWORD = "configureme";

String htmlEscape(const String &value) {
  String out = value;
  out.replace("&", "&amp;");
  out.replace("<", "&lt;");
  out.replace(">", "&gt;");
  out.replace("\"", "&quot;");
  out.replace("'", "&#39;");
  return out;
}

void loadConfig() {
  prefs.begin("printer", true);
  config.wifiSsid = prefs.getString("ssid", "");
  config.wifiPassword = prefs.getString("pass", "");
  config.printerName = prefs.getString("name", "HP Printer");
  config.printerModel = prefs.getString("model", "");
  config.printerIp = prefs.getString("ip", "");
  config.printerPort = prefs.getUShort("port", 9100);
  prefs.end();
}

void saveConfig() {
  prefs.begin("printer", false);
  prefs.putString("ssid", config.wifiSsid);
  prefs.putString("pass", config.wifiPassword);
  prefs.putString("name", config.printerName);
  prefs.putString("model", config.printerModel);
  prefs.putString("ip", config.printerIp);
  prefs.putUShort("port", config.printerPort);
  prefs.end();
}

void startAccessPoint() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.print("[AP] SSID: ");
  Serial.println(AP_SSID);
  Serial.print("[AP] Password: ");
  Serial.println(AP_PASSWORD);
  Serial.print("[AP] Configure at: http://");
  Serial.println(WiFi.softAPIP());
}

bool connectWiFi() {
  if (config.wifiSsid.isEmpty()) return false;

  WiFi.begin(config.wifiSsid.c_str(), config.wifiPassword.c_str());
  Serial.printf("[WiFi] Connecting to %s", config.wifiSsid.c_str());

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(500);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[WiFi] Connected: ");
    Serial.println(WiFi.localIP());
    return true;
  }

  Serial.println("[WiFi] Connection failed");
  return false;
}

String page() {
  String wifiState = WiFi.status() == WL_CONNECTED ? "Connected: " + WiFi.localIP().toString() : "Not connected";
  String apState = "http://" + WiFi.softAPIP().toString();

  String html = R"rawliteral(<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>
  <title>HP Print Server</title><style>
  body{font-family:Arial,sans-serif;max-width:720px;margin:30px auto;padding:0 16px;background:#f5f5f5;color:#222}
  section{background:white;padding:20px;margin:16px 0;border-radius:12px;box-shadow:0 2px 8px #0001}
  input{width:100%;box-sizing:border-box;padding:10px;margin:6px 0 14px;border:1px solid #bbb;border-radius:6px}
  button{padding:11px 18px;border:0;border-radius:7px;cursor:pointer}.save{background:#222;color:white}
  .status{padding:10px;background:#eee;border-radius:6px}small{color:#666}
  </style></head><body><h1>HP Print Server</h1>
  <section><h2>Network</h2><div class='status'>Wi-Fi: )rawliteral" + htmlEscape(wifiState) + R"rawliteral(</div>
  <p><small>Configuration AP: )rawliteral" + htmlEscape(apState) + R"rawliteral(</small></p>
  <form method='POST' action='/save'>
  <label>Wi-Fi SSID</label><input name='ssid' value=')rawliteral" + htmlEscape(config.wifiSsid) + R"rawliteral('>
  <label>Wi-Fi password</label><input type='password' name='password' placeholder='Leave unchanged to keep current password'>
  <button class='save' type='submit'>Save &amp; Connect</button></form></section>
  <section><h2>Printer</h2><form method='POST' action='/save'>
  <label>Printer name</label><input name='printerName' value=')rawliteral" + htmlEscape(config.printerName) + R"rawliteral('>
  <label>Printer model</label><input name='printerModel' value=')rawliteral" + htmlEscape(config.printerModel) + R"rawliteral(' placeholder='Example: HP Smart Tank 520'>
  <label>Printer IP</label><input name='printerIp' value=')rawliteral" + htmlEscape(config.printerIp) + R"rawliteral(' placeholder='Optional; leave blank for USB'>
  <label>Printer TCP port</label><input type='number' name='printerPort' value=')rawliteral" + String(config.printerPort) + R"rawliteral('>
  <input type='hidden' name='ssid' value=')rawliteral" + htmlEscape(config.wifiSsid) + R"rawliteral('>
  <button class='save' type='submit'>Save printer settings</button></form></section>
  <section><h2>Server</h2><div class='status'>IPP: Not implemented yet<br>USB Host: Not implemented yet</div></section>
  </body></html>)rawliteral";
  return html;
}

void handleRoot() { server.send(200, "text/html", page()); }

void handleSave() {
  if (server.hasArg("ssid")) config.wifiSsid = server.arg("ssid");
  if (server.hasArg("password") && server.arg("password").length()) config.wifiPassword = server.arg("password");
  if (server.hasArg("printerName")) config.printerName = server.arg("printerName");
  if (server.hasArg("printerModel")) config.printerModel = server.arg("printerModel");
  if (server.hasArg("printerIp")) config.printerIp = server.arg("printerIp");
  if (server.hasArg("printerPort")) {
    int port = server.arg("printerPort").toInt();
    if (port > 0 && port <= 65535) config.printerPort = (uint16_t)port;
  }

  saveConfig();
  server.send(200, "text/html", "<meta http-equiv='refresh' content='3;url=/'><p>Saved. Attempting Wi-Fi connection...</p>");

  if (WiFi.status() != WL_CONNECTED) connectWiFi();
}

void handleHealth() {
  server.send(200, "text/plain", WiFi.status() == WL_CONNECTED ? "OK\n" : "CONFIGURATION_REQUIRED\n");
}

void handleNotFound() { server.send(404, "text/plain", "Not found\n"); }

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== HP Print Server / ESP32-S3 ===");

  loadConfig();
  startAccessPoint();
  connectWiFi();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/health", HTTP_GET, handleHealth);
  server.on("/save", HTTP_POST, handleSave);
  server.onNotFound(handleNotFound);
  server.begin();

  Serial.println("[HTTP] Configuration server started");

  // Future: mDNS/DNS-SD, IPP server, USB Host and HP backend.
}

void loop() {
  server.handleClient();
  delay(2);
}
