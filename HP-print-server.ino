#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include "printer_protocols.h"
#include "mobile_print_profile.h"
#include "mobile_print_queue.h"
#include "mobile_ipp_server.h"

WebServer configServer(80);
Preferences preferences;
MobilePrintQueue printQueue;
MobileIppServer ippServer(MobilePrintProfile::IPP_PORT);

static constexpr const char *CONFIG_NS = "hp-print";
static constexpr const char *AP_SSID = "HP-Print-Server";
static constexpr const char *AP_PASSWORD = "configureme";
static constexpr const char *HOSTNAME = "hp-print-server";

struct Config { String ssid; String password; String printerName; String printerModel; };
Config config;

void defaults() {
  config.ssid = ""; config.password = "";
  config.printerName = "HP Print Server";
  config.printerModel = "HP Smart Tank 520";
}

void loadConfig() {
  defaults();
  if (!preferences.begin(CONFIG_NS, true)) return;
  config.ssid = preferences.getString("ssid", config.ssid);
  config.password = preferences.getString("pass", config.password);
  config.printerName = preferences.getString("name", config.printerName);
  config.printerModel = preferences.getString("model", config.printerModel);
  preferences.end();
}

bool saveConfig() {
  if (!preferences.begin(CONFIG_NS, false)) return false;
  bool ok = true;
  ok &= preferences.putString("ssid", config.ssid) > 0 || config.ssid.isEmpty();
  ok &= preferences.putString("pass", config.password) > 0 || config.password.isEmpty();
  ok &= preferences.putString("name", config.printerName) > 0;
  ok &= preferences.putString("model", config.printerModel) > 0;
  preferences.end();
  return ok;
}

String esc(String s) {
  s.replace("&", "&amp;"); s.replace("<", "&lt;"); s.replace(">", "&gt;");
  s.replace("\"", "&quot;"); s.replace("'", "&#39;");
  return s;
}

bool connectWiFi() {
  if (config.ssid.isEmpty()) return false;
  WiFi.begin(config.ssid.c_str(), config.password.c_str());
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000UL) delay(250);
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[WiFi] Connected: "); Serial.println(WiFi.localIP());
    return true;
  }
  Serial.println("[WiFi] Connection failed; configuration AP remains available");
  return false;
}

void startConfigAP() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASSWORD, 1, false, 4);
  Serial.print("[AP] Configure at http://"); Serial.println(WiFi.softAPIP());
}

void advertiseMobilePrinter() {
  if (WiFi.status() != WL_CONNECTED) return;
  MDNS.end();
  if (!MDNS.begin(HOSTNAME)) return;
  // Android Default Print Service/Mopria expects DNS-SD _ipp._tcp records and
  // uses the rp TXT record as the IPP resource path.
  MDNS.addService("ipp", "tcp", MobilePrintProfile::IPP_PORT);
  MDNS.addServiceTxt("ipp", "tcp", "txtvers", MobilePrintProfile::TXT_VERS);
  MDNS.addServiceTxt("ipp", "tcp", "qtotal", "1");
  MDNS.addServiceTxt("ipp", "tcp", "rp", "ipp/print");
  MDNS.addServiceTxt("ipp", "tcp", "ty", config.printerName.c_str());
  MDNS.addServiceTxt("ipp", "tcp", "product", MobilePrintProfile::TXT_PRODUCT);
  MDNS.addServiceTxt("ipp", "tcp", "note", MobilePrintProfile::TXT_NOTE);
  MDNS.addServiceTxt("ipp", "tcp", "adminurl", "http://hp-print-server.local/");
  MDNS.addServiceTxt("ipp", "tcp", "pdl", "image/pwg-raster,application/PCLm,application/pdf,image/jpeg,image/urf");
  MDNS.addServiceTxt("ipp", "tcp", "priority", "0");
  Serial.print("[mDNS] "); Serial.print(HOSTNAME); Serial.println(".local advertising _ipp._tcp");
}

String printerUri() {
  return String("ipp://") + HOSTNAME + ".local:" + String(MobilePrintProfile::IPP_PORT) + MobilePrintProfile::IPP_PATH;
}

bool handleMobileJob(const uint8_t *document, size_t length, const String &format,
                     uint32_t &jobId, String &error) {
  if (format != MobilePrintProfile::FORMAT_PWG && format != MobilePrintProfile::FORMAT_PCLM &&
      format != MobilePrintProfile::FORMAT_PDF && format != MobilePrintProfile::FORMAT_JPEG &&
      format != MobilePrintProfile::FORMAT_URF) {
    error = "Unsupported mobile document format: " + format;
    return false;
  }
  if (printQueue.hasJob()) { error = "Printer busy: one print job is already queued"; return false; }
  if (!printQueue.enqueue(document, length, format, jobId, error)) return false;
  Serial.printf("[IPP] Accepted job %lu: %u bytes, %s\n", (unsigned long)jobId, (unsigned)length, format.c_str());
  return true;
}

String dashboard() {
  String wifi = WiFi.status() == WL_CONNECTED ? "Connected — " + WiFi.localIP().toString() : "Configuration required — AP 192.168.4.1";
  String html = R"rawliteral(<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'><title>HP Print Server</title><style>body{font-family:system-ui,Arial;max-width:760px;margin:24px auto;padding:0 16px;background:#f5f5f5;color:#222}section{background:#fff;padding:20px;margin:16px 0;border-radius:12px;box-shadow:0 2px 8px #0001}input{width:100%;box-sizing:border-box;padding:10px;margin:6px 0 14px;border:1px solid #aaa;border-radius:7px}button{padding:11px 18px;border:0;border-radius:7px;background:#222;color:#fff}.status{padding:12px;background:#eee;border-radius:7px}</style></head><body><h1>HP Print Server</h1><section><h2>Status</h2><div class='status'>Wi-Fi: )rawliteral" + esc(wifi) + R"rawliteral(<br>IPP: )rawliteral" + String(ippServer.running() ? "ready" : "offline") + R"rawliteral(<br>Printer: )rawliteral" + esc(config.printerName) + R"rawliteral(<br>Model: )rawliteral" + esc(config.printerModel) + R"rawliteral(<br>Queued job: )rawliteral" + String(printQueue.hasJob() ? "yes" : "no") + R"rawliteral(</div></section><section><h2>Wi-Fi</h2><form method='POST' action='/save'><label>SSID</label><input name='ssid' value=')rawliteral" + esc(config.ssid) + R"rawliteral(' maxlength='32'><label>Password</label><input type='password' name='password' placeholder='Leave blank to keep current password'><button>Save &amp; restart</button></form><p><a href='/scan'>Scan nearby networks</a></p></section><section><h2>Printer</h2><form method='POST' action='/save'><label>Printer name</label><input name='printerName' value=')rawliteral" + esc(config.printerName) + R"rawliteral('><label>Printer model</label><input name='printerModel' value=')rawliteral" + esc(config.printerModel) + R"rawliteral('><button>Save printer information</button></form></section><section><h2>Mobile printing</h2><p>Android Default Print Service / Mopria discovery: <b>DNS-SD + IPP</b></p><p>IPP endpoint: <code>)rawliteral" + printerUri() + R"rawliteral(</code></p><p>Accepted mobile formats: PWG Raster, PCLm, PDF, JPEG, URF.</p><p>The current firmware persistently queues the incoming job. The HP Smart Tank 520 USB/PCLm renderer and USB Host transport are the printer-side layer still to be completed.</p></section></body></html>)rawliteral";
  return html;
}

void handleRoot() { configServer.send(200, "text/html; charset=utf-8", dashboard()); }

void handleSave() {
  if (configServer.hasArg("ssid")) config.ssid = configServer.arg("ssid");
  if (configServer.hasArg("password") && !configServer.arg("password").isEmpty()) config.password = configServer.arg("password");
  if (configServer.hasArg("printerName")) config.printerName = configServer.arg("printerName");
  if (configServer.hasArg("printerModel")) config.printerModel = configServer.arg("printerModel");
  if (!saveConfig()) { configServer.send(500, "text/plain", "Configuration save failed\n"); return; }
  configServer.send(200, "text/html", "<p>Saved. Restarting...</p>"); delay(250); ESP.restart();
}

void handleScan() {
  int n = WiFi.scanNetworks(false, true);
  String html = "<html><head><meta name='viewport' content='width=device-width,initial-scale=1'></head><body><h1>Wi-Fi networks</h1><ul>";
  for (int i = 0; i < n; ++i) { String ssid = WiFi.SSID(i); if (ssid.isEmpty()) ssid = "(hidden)"; html += "<li>" + esc(ssid) + " — " + String(WiFi.RSSI(i)) + " dBm — ch " + String(WiFi.channel(i)) + "</li>"; }
  html += "</ul><a href='/'>Back</a></body></html>";
  WiFi.scanDelete(); configServer.send(200, "text/html; charset=utf-8", html);
}

void handleHealth() {
  String body = "wifi=" + String(WiFi.status() == WL_CONNECTED ? "connected" : "not-connected") + "\n";
  body += "ipp=" + String(ippServer.running() ? "ready" : "offline") + "\n";
  body += "queued=" + String(printQueue.hasJob() ? "yes" : "no") + "\n";
  if (printQueue.hasJob()) body += "job_bytes=" + String(printQueue.jobSize()) + "\n";
  configServer.send(200, "text/plain", body);
}

void setup() {
  Serial.begin(115200); delay(500);
  Serial.println("\n=== HP Print Server / ESP32-S3 / Mobile-first ===");
  loadConfig();
  if (!printQueue.begin()) Serial.println("[Queue] Persistent queue unavailable");
  startConfigAP();
  bool wifi = connectWiFi();
  if (wifi) { advertiseMobilePrinter(); ippServer.begin(config.printerName, printerUri(), handleMobileJob); }
  configServer.on("/", HTTP_GET, handleRoot);
  configServer.on("/scan", HTTP_GET, handleScan);
  configServer.on("/health", HTTP_GET, handleHealth);
  configServer.on("/save", HTTP_POST, handleSave);
  configServer.begin();
  Serial.println("[HTTP] Configuration server ready");
  if (!wifi) Serial.println("[HTTP] Connect to HP-Print-Server and open http://192.168.4.1/");
}

void loop() { configServer.handleClient(); ippServer.poll(); delay(1); }
