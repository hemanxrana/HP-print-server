#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include "printer_protocols.h"

WebServer server(80);
Preferences prefs;
WiFiUDP udp;

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
  bool directApEnabled;
};

Config config;

const char *AP_SSID = "HP-Print-Server";
const char *AP_PASSWORD = "configureme";
const char *DIRECT_AP_SSID = "DIRECT-XX-HP Print Server";

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
  config.printerName = prefs.getString("name", "HP Print Server");
  config.printerModel = prefs.getString("model", "");
  config.printerIp = prefs.getString("ip", "");
  config.printerHost = prefs.getString("host", "");
  config.lprQueue = prefs.getString("lprq", "lp");
  config.printerPort = prefs.getUShort("port", 9100);
  config.transport = (PrinterTransport)prefs.getUChar("transport", TRANSPORT_JETDIRECT_RAW);
  config.discovery = (PrinterDiscovery)prefs.getUChar("discovery", DISCOVERY_MDNS);
  config.directApEnabled = prefs.getBool("directap", false);
  prefs.end();
}

void saveConfig() {
  prefs.begin("printer", false);
  prefs.putString("ssid", config.wifiSsid);
  prefs.putString("pass", config.wifiPassword);
  prefs.putString("name", config.printerName);
  prefs.putString("model", config.printerModel);
  prefs.putString("ip", config.printerIp);
  prefs.putString("host", config.printerHost);
  prefs.putString("lprq", config.lprQueue);
  prefs.putUShort("port", config.printerPort);
  prefs.putUChar("transport", (uint8_t)config.transport);
  prefs.putUChar("discovery", (uint8_t)config.discovery);
  prefs.putBool("directap", config.directApEnabled);
  prefs.end();
}

void startAccessPoint() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.print("[AP] Configuration SSID: ");
  Serial.println(AP_SSID);
  Serial.print("[AP] Password: ");
  Serial.println(AP_PASSWORD);
  Serial.print("[AP] Configure at: http://");
  Serial.println(WiFi.softAPIP());

  if (config.directApEnabled) {
    // This is a Wi-Fi Direct-style compatibility AP. It is not a full
    // Wi-Fi Alliance Wi-Fi Direct implementation; it provides a direct
    // wireless network for Android/manual testing without a router.
    WiFi.softAP(DIRECT_AP_SSID, AP_PASSWORD, 6, false, 4);
    Serial.print("[Direct] SSID: ");
    Serial.println(DIRECT_AP_SSID);
    Serial.print("[Direct] IP: ");
    Serial.println(WiFi.softAPIP());
  }
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

  Serial.println("[WiFi] Connection failed; configuration AP remains available");
  return false;
}

void startMDNS() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (!MDNS.begin("hp-print-server")) {
    Serial.println("[mDNS] Start failed");
    return;
  }

  // Advertise the protocols we intend to support. The actual IPP service
  // will be attached here when the IPP server is implemented.
  MDNS.addService("ipp", "tcp", 631);
  MDNS.addService("ipps", "tcp", 443);
  MDNS.addService("printer", "tcp", 9100);
  MDNS.addServiceTxt("ipp", "tcp", "ty", config.printerName.c_str());
  MDNS.addServiceTxt("ipp", "tcp", "product", config.printerModel.c_str());
  Serial.println("[mDNS] Printer discovery advertisement started");
}

String scanWifiHtml() {
  int count = WiFi.scanNetworks(false, true);
  String result = "<h3>Nearby Wi-Fi networks</h3><select name='scanSsid'>";
  if (count <= 0) {
    result += "<option value=''>No networks found</option>";
  } else {
    for (int i = 0; i < count; ++i) {
      String ssid = WiFi.SSID(i);
      result += "<option value='" + htmlEscape(ssid) + "'>" + htmlEscape(ssid) + " (" + String(WiFi.RSSI(i)) + " dBm)</option>";
    }
  }
  result += "</select>";
  WiFi.scanDelete();
  return result;
}

String transportOptions() {
  String s;
  for (uint8_t i = TRANSPORT_USB; i <= TRANSPORT_HTTPS; ++i) {
    s += "<option value='" + String(i) + "'" + (config.transport == i ? " selected" : "") + ">" + transportName((PrinterTransport)i) + "</option>";
  }
  return s;
}

String discoveryOptions() {
  String s;
  for (uint8_t i = DISCOVERY_MANUAL_IP; i <= DISCOVERY_WIFI_DIRECT; ++i) {
    s += "<option value='" + String(i) + "'" + (config.discovery == i ? " selected" : "") + ">" + discoveryName((PrinterDiscovery)i) + "</option>";
  }
  return s;
}

String page() {
  String wifiState = WiFi.status() == WL_CONNECTED ? "Connected: " + WiFi.localIP().toString() : "Not connected";
  String html = R"rawliteral(<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>
  <title>HP Print Server</title><style>
  body{font-family:Arial,sans-serif;max-width:820px;margin:25px auto;padding:0 16px;background:#f5f5f5;color:#222}
  section{background:white;padding:20px;margin:16px 0;border-radius:12px;box-shadow:0 2px 8px #0001}
  input,select{width:100%;box-sizing:border-box;padding:10px;margin:6px 0 14px;border:1px solid #bbb;border-radius:6px}
  button{padding:11px 18px;border:0;border-radius:7px;cursor:pointer;margin:3px}.save{background:#222;color:white}.scan{background:#ddd}
  .status{padding:10px;background:#eee;border-radius:6px}.grid{display:grid;grid-template-columns:1fr 1fr;gap:14px}@media(max-width:650px){.grid{grid-template-columns:1fr}}
  small{color:#666}.proto{font-family:monospace;background:#f1f1f1;padding:3px 5px;border-radius:4px}
  </style></head><body><h1>HP Print Server</h1>
  <section><h2>Network</h2><div class='status'>Wi-Fi: )rawliteral" + htmlEscape(wifiState) + R"rawliteral(</div>
  <form method='POST' action='/save'>
  <label>Wi-Fi SSID</label><input name='ssid' value=')rawliteral" + htmlEscape(config.wifiSsid) + R"rawliteral('>
  <label>Wi-Fi password</label><input type='password' name='password' placeholder='Leave unchanged to keep current password'>
  <button class='save' type='submit'>Save &amp; Connect</button></form>
  <form method='GET' action='/scan'><button class='scan'>Scan nearby Wi-Fi networks</button></form></section>

  <section><h2>Printer identity</h2><div class='grid'>
  <div><label>Printer name</label><input name='printerName' form='printerform' value=')rawliteral" + htmlEscape(config.printerName) + R"rawliteral('></div>
  <div><label>Printer model</label><input name='printerModel' form='printerform' value=')rawliteral" + htmlEscape(config.printerModel) + R"rawliteral(' placeholder='Example: HP Smart Tank 520'></div></div>
  <form id='printerform' method='POST' action='/save'>
  <label>Discovery method</label><select name='discovery'>)rawliteral" + discoveryOptions() + R"rawliteral(</select>
  <label>Print transport</label><select name='transport'>)rawliteral" + transportOptions() + R"rawliteral(</select>
  <label>Printer IP / hostname</label><input name='printerIp' value=')rawliteral" + htmlEscape(config.printerIp) + R"rawliteral(' placeholder='Optional when using discovery'>
  <label>Printer hostname</label><input name='printerHost' value=')rawliteral" + htmlEscape(config.printerHost) + R"rawliteral(' placeholder='Example: printer.local'>
  <div class='grid'><div><label>TCP port</label><input type='number' name='printerPort' value=')rawliteral" + String(config.printerPort) + R"rawliteral('></div>
  <div><label>LPR queue</label><input name='lprQueue' value=')rawliteral" + htmlEscape(config.lprQueue) + R"rawliteral('></div></div>
  <label><input type='checkbox' name='directAp' value='1' style='width:auto' )rawliteral" + String(config.directApEnabled ? "checked" : "") + R"rawliteral(> Enable direct-wireless compatibility AP</label><br>
  <button class='save' type='submit'>Save printer settings</button></form></section>

  <section><h2>Supported discovery paths</h2>
  <p><span class='proto'>mDNS / DNS-SD</span> IPP/Bonjour-style service discovery</p>
  <p><span class='proto'>WS-Discovery</span> SOAP probe on the local network</p>
  <p><span class='proto'>SSDP / UPnP</span> UDP multicast discovery</p>
  <p><span class='proto'>SNMP</span> printer identification/status probing</p>
  <p><span class='proto'>HTTP/HTTPS</span> Embedded Web Server probing</p>
  <p><span class='proto'>Wi-Fi Direct</span> direct AP compatibility mode</p>
  <p><span class='proto'>Manual IP</span> deterministic fallback</p></section>

  <section><h2>Supported print transports</h2>
  <p>IPP (631), IPPS, JetDirect/RAW (9100), LPR/LPD (515), WSD, HTTP/HTTPS and USB Host.</p>
  <div class='status'>IPP/WSD/RAW/LPR/USB data paths are being implemented incrementally. The current firmware only advertises the architecture and configuration; it does not yet claim to print through all of these transports.</div></section>
  </body></html>)rawliteral";
  return html;
}

void handleRoot() { server.send(200, "text/html", page()); }

void handleScan() {
  // Scanning is intentionally a separate request because Wi-Fi scanning can
  // take hundreds of milliseconds and should not block the normal dashboard.
  server.send(200, "text/html", "<html><body><h1>Wi-Fi scan</h1>" + scanWifiHtml() + "<p><a href='/'>Back</a></p></body></html>");
}

void handleSave() {
  if (server.hasArg("ssid")) config.wifiSsid = server.arg("ssid");
  if (server.hasArg("password") && server.arg("password").length()) config.wifiPassword = server.arg("password");
  if (server.hasArg("printerName")) config.printerName = server.arg("printerName");
  if (server.hasArg("printerModel")) config.printerModel = server.arg("printerModel");
  if (server.hasArg("printerIp")) config.printerIp = server.arg("printerIp");
  if (server.hasArg("printerHost")) config.printerHost = server.arg("printerHost");
  if (server.hasArg("lprQueue")) config.lprQueue = server.arg("lprQueue");
  if (server.hasArg("printerPort")) {
    int port = server.arg("printerPort").toInt();
    if (port > 0 && port <= 65535) config.printerPort = (uint16_t)port;
  }
  if (server.hasArg("transport")) {
    int value = server.arg("transport").toInt();
    if (value >= TRANSPORT_USB && value <= TRANSPORT_HTTPS) config.transport = (PrinterTransport)value;
  }
  if (server.hasArg("discovery")) {
    int value = server.arg("discovery").toInt();
    if (value >= DISCOVERY_MANUAL_IP && value <= DISCOVERY_WIFI_DIRECT) config.discovery = (PrinterDiscovery)value;
  }
  config.directApEnabled = server.hasArg("directAp");

  saveConfig();
  server.send(200, "text/html", "<meta http-equiv='refresh' content='3;url=/'><p>Saved. Reconnecting/configuring...</p>");

  if (WiFi.status() != WL_CONNECTED) connectWiFi();
  startMDNS();
}

void handleHealth() {
  String status = WiFi.status() == WL_CONNECTED ? "OK" : "CONFIGURATION_REQUIRED";
  status += "\ntransport=" + String(transportName(config.transport));
  status += "\ndiscovery=" + String(discoveryName(config.discovery));
  server.send(200, "text/plain", status + "\n");
}

void handleNotFound() { server.send(404, "text/plain", "Not found\n"); }

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== HP Print Server / ESP32-S3 ===");

  loadConfig();
  startAccessPoint();
  connectWiFi();
  startMDNS();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/scan", HTTP_GET, handleScan);
  server.on("/health", HTTP_GET, handleHealth);
  server.on("/save", HTTP_POST, handleSave);
  server.onNotFound(handleNotFound);
  server.begin();

  Serial.println("[HTTP] Configuration server started");
  Serial.println("[Discovery] mDNS advertisement enabled when STA is connected");
  Serial.println("[Discovery] WSD/SSDP/SNMP/HTTP probing will be added to the discovery engine");
}

void loop() {
  server.handleClient();
  delay(2);
}
