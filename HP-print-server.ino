#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include "printer_protocols.h"
#include "mobile_print_profile.h"
#include "mobile_print_queue.h"
#include "mobile_ipp_server.h"
#include "usb_printer_backend.h"

WebServer configServer(80);
Preferences preferences;
MobilePrintQueue printQueue;
MobileIppServer ippServer(MobilePrintProfile::IPP_PORT);
UsbHostManager usbHost;
UsbPrinterBackend usbPrinterBackend(usbHost);

static constexpr const char *CONFIG_NS = "hp-print";
static constexpr const char *AP_SSID = "HP-Print-Server";
static constexpr const char *AP_PASSWORD = "configureme";
static constexpr const char *HOSTNAME = "hp-print-server";

struct Config {
  String ssid;
  String password;
  String printerName;
  String printerModel;
  bool usbAuto = true;
  uint8_t usbInterface = 0;
  uint8_t usbAlt = 0;
};

Config config;

void defaults() {
  config.ssid = "";
  config.password = "";
  config.printerName = "HP Print Server";
  config.printerModel = "HP Smart Tank 520";
  config.usbAuto = true;
  config.usbInterface = 0;
  config.usbAlt = 0;
}

void loadConfig() {
  defaults();
  if (!preferences.begin(CONFIG_NS, true)) return;
  config.ssid = preferences.getString("ssid", config.ssid);
  config.password = preferences.getString("pass", config.password);
  config.printerName = preferences.getString("name", config.printerName);
  config.printerModel = preferences.getString("model", config.printerModel);
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
  ok &= preferences.putString("name", config.printerName) > 0;
  ok &= preferences.putString("model", config.printerModel) > 0;
  ok &= preferences.putBool("usbauto", config.usbAuto);
  ok &= preferences.putUChar("usbif", config.usbInterface) > 0;
  ok &= preferences.putUChar("usbalt", config.usbAlt) > 0;
  preferences.end();
  return ok;
}

String esc(String s) {
  s.replace("&", "&amp;");
  s.replace("<", "&lt;");
  s.replace(">", "&gt;");
  s.replace("\"", "&quot;");
  s.replace("'", "&#39;");
  return s;
}

bool connectWiFi() {
  if (config.ssid.isEmpty()) {
    Serial.println("[WiFi] No saved SSID");
    return false;
  }

  // Do not start the configuration AP first. AP+STA fixes the radio to the
  // AP channel and can prevent a connection to a router using another channel.
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
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
  Serial.println(HOSTNAME);
  return true;
}

bool startConfigAP() {
  // AP is a fallback/configuration network only. It is not used when the
  // saved router connection succeeds.
  WiFi.mode(WIFI_AP);
  WiFi.setHostname(HOSTNAME);
  const bool ok = WiFi.softAP(AP_SSID, AP_PASSWORD, 1, false, 4);
  if (!ok) {
    Serial.println("[AP] Failed to start configuration AP");
    return false;
  }
  Serial.print("[AP] SSID: ");
  Serial.println(AP_SSID);
  Serial.print("[AP] Configure at http://");
  Serial.println(WiFi.softAPIP());
  return true;
}

bool advertiseMobilePrinter() {
  MDNS.end();
  if (!MDNS.begin(HOSTNAME)) {
    Serial.println("[mDNS] Failed to start mDNS responder");
    return false;
  }

  const String pdl = "image/pwg-raster,application/PCLm,application/pdf,image/jpeg,image/urf";
  const String uuid = String("esp32-") + WiFi.macAddress();
  const String admin = String("http://") + HOSTNAME + ".local/";

  // Android Default Print Service / Mopria discovers ordinary IPP printers
  // through _ipp._tcp DNS-SD. These are the standard printer TXT attributes.
  MDNS.addService("ipp", "tcp", MobilePrintProfile::IPP_PORT);
  MDNS.addServiceTxt("ipp", "tcp", "txtvers", "1");
  MDNS.addServiceTxt("ipp", "tcp", "qtotal", String(MobilePrintQueue::MAX_JOBS).c_str());
  MDNS.addServiceTxt("ipp", "tcp", "rp", "ipp/print");
  MDNS.addServiceTxt("ipp", "tcp", "ty", config.printerModel.c_str());
  MDNS.addServiceTxt("ipp", "tcp", "product", (String("(") + config.printerModel + ")").c_str());
  MDNS.addServiceTxt("ipp", "tcp", "note", "USB print server for HP printers");
  MDNS.addServiceTxt("ipp", "tcp", "adminurl", admin.c_str());
  MDNS.addServiceTxt("ipp", "tcp", "priority", "0");
  MDNS.addServiceTxt("ipp", "tcp", "pdl", pdl.c_str());
  MDNS.addServiceTxt("ipp", "tcp", "URF", "W8,SRGB24,CP255,RS300-600,DM1");
  MDNS.addServiceTxt("ipp", "tcp", "mopria-certified", "2.0");
  MDNS.addServiceTxt("ipp", "tcp", "UUID", uuid.c_str());
  MDNS.addServiceTxt("ipp", "tcp", "printer-state", "3");
  MDNS.addServiceTxt("ipp", "tcp", "kind", "document,photo");

  // Legacy printer discovery is useful to HP/Mopria clients as well. The
  // service itself does not carry print traffic; IPP remains the print path.
  MDNS.addService("printer", "tcp", 0);

  Serial.print("[mDNS] ");
  Serial.print(config.printerName);
  Serial.println(" advertising _ipp._tcp on TCP 631");
  return true;
}

String printerUri() {
  return String("ipp://") + HOSTNAME + ".local:" +
         String(MobilePrintProfile::IPP_PORT) + MobilePrintProfile::IPP_PATH;
}

bool handleMobileJob(const uint8_t *document, size_t length,
                     const String &format, uint32_t &jobId, String &error) {
  if (!printQueue.enqueue(document, length, format, jobId, error)) return false;
  Serial.printf("[IPP] Accepted job %lu: %u bytes, %s\n",
                (unsigned long)jobId, (unsigned)length, format.c_str());
  return true;
}

String usbInterfaceLabel(const UsbPrinterInterfaceInfo &p, bool selected) {
  String s = "IF " + String(p.interfaceNumber) + " / ALT " + String(p.alternateSetting) +
             " — protocol 0x";
  if (p.protocol < 16) s += "0";
  s += String(p.protocol, HEX);
  s += " — OUT 0x";
  if (p.bulkOut.address < 16) s += "0";
  s += String(p.bulkOut.address, HEX);
  s += " — IN ";
  if (p.bulkIn.valid()) {
    s += "0x";
    if (p.bulkIn.address < 16) s += "0";
    s += String(p.bulkIn.address, HEX);
  } else {
    s += "none";
  }
  if (selected) s += " [ACTIVE]";
  return s;
}

String dashboard() {
  String wifi;
  if (WiFi.status() == WL_CONNECTED) {
    wifi = "Connected — " + WiFi.localIP().toString();
  } else if (WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA) {
    wifi = "Configuration AP — " + WiFi.softAPIP().toString();
  } else {
    wifi = "Not connected";
  }

  String usbState;
  switch (usbHost.state()) {
    case UsbHostManager::PRINTER_READY: usbState = "Printer Class ready"; break;
    case UsbHostManager::ERROR: usbState = "USB error: " + usbHost.lastError(); break;
    case UsbHostManager::RUNNING: usbState = "USB host running — waiting for printer"; break;
    default: usbState = "USB host starting/enumerating"; break;
  }

  String usbOptions;
  if (!usbHost.device().attached || usbHost.interfaceCount() == 0) {
    usbOptions = "<p>No printer interfaces detected yet. Connect the USB printer and refresh this page.</p>";
  } else {
    usbOptions += "<form method='POST' action='/usb'><label><input type='radio' name='mode' value='auto' ";
    usbOptions += config.usbAuto ? "checked" : "";
    usbOptions += "> Automatic — prefer standard bidirectional Printer Class (protocol 0x02)</label><br><br>";
    for (uint8_t i = 0; i < usbHost.interfaceCount(); ++i) {
      const UsbPrinterInterfaceInfo *p = usbHost.interfaceAt(i);
      if (!p) continue;
      const bool selected = !config.usbAuto &&
                            p->interfaceNumber == config.usbInterface &&
                            p->alternateSetting == config.usbAlt;
      usbOptions += "<label><input type='radio' name='mode' value='manual:" +
                    String(p->interfaceNumber) + ":" + String(p->alternateSetting) + "' ";
      usbOptions += selected ? "checked" : "";
      usbOptions += "> " + esc(usbInterfaceLabel(*p,
          p->interfaceNumber == usbHost.device().printer.interfaceNumber &&
          p->alternateSetting == usbHost.device().printer.alternateSetting)) +
          "</label><br>";
    }
    usbOptions += "<br><button>Apply USB interface</button></form>";
  }

  String selected = usbHost.selectedInterface()
      ? usbInterfaceLabel(*usbHost.selectedInterface(), true) : "none";

  String html = R"rawliteral(<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>
<title>HP Print Server</title><style>body{font-family:system-ui,Arial;max-width:800px;margin:24px auto;padding:0 16px;background:#f5f5f5;color:#222}section{background:#fff;padding:20px;margin:16px 0;border-radius:12px;box-shadow:0 2px 8px #0001}input{width:100%;box-sizing:border-box;padding:10px;margin:6px 0 14px;border:1px solid #aaa;border-radius:7px}input[type=radio]{width:auto;margin:6px 8px 6px 0}button{padding:11px 18px;border:0;border-radius:7px;background:#222;color:#fff;margin:5px 4px 5px 0}.status{padding:12px;background:#eee;border-radius:7px}code{word-break:break-all}</style></head><body>
<h1>HP Print Server</h1><section><h2>Status</h2><div class='status'>Wi-Fi: )rawliteral" + esc(wifi) + R"rawliteral(<br>IPP: )rawliteral" +
    String(ippServer.running() ? "ready" : "offline") + R"rawliteral(<br>USB: )rawliteral" + esc(usbState) +
    R"rawliteral(<br>Printer: )rawliteral" + esc(config.printerName) + R"rawliteral(<br>Model: )rawliteral" +
    esc(config.printerModel) + R"rawliteral(<br>Active jobs: )rawliteral" + String(printQueue.activeCount()) +
    " / " + String(MobilePrintQueue::MAX_JOBS) + R"rawliteral(<br>Retained jobs: )rawliteral" +
    String(printQueue.count()) + R"rawliteral(</div></section><section><h2>Wi-Fi</h2><form method='POST' action='/save'>
<label>SSID</label><input name='ssid' value=')rawliteral" + esc(config.ssid) + R"rawliteral(' maxlength='32'>
<label>Password</label><input type='password' name='password' placeholder='Leave blank to keep current password'>
<button>Save &amp; restart</button></form><p><a href='/scan'>Scan nearby networks</a></p></section>
<section><h2>Printer</h2><form method='POST' action='/save'><label>Printer name</label><input name='printerName' value=')rawliteral" +
    esc(config.printerName) + R"rawliteral('><label>Printer model</label><input name='printerModel' value=')rawliteral" +
    esc(config.printerModel) + R"rawliteral('><button>Save printer information</button></form></section>
<section><h2>USB Printer Interface</h2><p>Detected device: )rawliteral" +
    (usbHost.device().attached ? String("VID 0x") + String(usbHost.device().vid, HEX) + " / PID 0x" + String(usbHost.device().pid, HEX) : String("none")) +
    R"rawliteral(</p><p>Active: <b>)rawliteral" + esc(selected) + R"rawliteral(</b></p>)rawliteral" + usbOptions + R"rawliteral(
<form method='POST' action='/usb/test'><button>Test Print</button></form><p>Test Print uses the production USB backend and selected interface.</p></section>
<section><h2>Mobile printing</h2><p>Android Default Print Service / Mopria discovery: <b>DNS-SD + IPP</b></p>
<p>IPP endpoint: <code>)rawliteral" + printerUri() + R"rawliteral(</code></p><p>Accepted formats: PWG Raster, PCLm, PDF, JPEG, URF.</p>
<p>Phone and ESP32 must be on the same Wi-Fi network for router-based discovery.</p></section></body></html>)rawliteral";
  return html;
}

void handleRoot() {
  configServer.send(200, "text/html; charset=utf-8", dashboard());
}

void handleSave() {
  if (configServer.hasArg("ssid")) config.ssid = configServer.arg("ssid");
  if (configServer.hasArg("password") && !configServer.arg("password").isEmpty())
    config.password = configServer.arg("password");
  if (configServer.hasArg("printerName")) config.printerName = configServer.arg("printerName");
  if (configServer.hasArg("printerModel")) config.printerModel = configServer.arg("printerModel");

  if (!saveConfig()) {
    configServer.send(500, "text/plain", "Configuration save failed\n");
    return;
  }
  configServer.send(200, "text/html", "<p>Saved. Restarting...</p>");
  delay(250);
  ESP.restart();
}

void handleUsbSave() {
  if (!configServer.hasArg("mode")) {
    configServer.send(400, "text/plain", "Missing USB interface mode\n");
    return;
  }

  String mode = configServer.arg("mode");
  if (mode == "auto") {
    config.usbAuto = true;
  } else if (mode.startsWith("manual:")) {
    int first = mode.indexOf(':');
    int second = mode.indexOf(':', first + 1);
    if (second < 0) {
      configServer.send(400, "text/plain", "Invalid USB interface selection\n");
      return;
    }
    config.usbAuto = false;
    config.usbInterface = (uint8_t)mode.substring(first + 1, second).toInt();
    config.usbAlt = (uint8_t)mode.substring(second + 1).toInt();
  } else {
    configServer.send(400, "text/plain", "Invalid USB interface mode\n");
    return;
  }

  usbHost.setInterfaceSelection(config.usbAuto, config.usbInterface, config.usbAlt);
  if (!saveConfig()) {
    configServer.send(500, "text/plain", "USB configuration save failed\n");
    return;
  }
  configServer.send(200, "text/html", "<p>USB interface selection applied.</p><p><a href='/'>Back</a></p>");
}

void handleUsbTestPrint() {
  String error;
  if (!usbPrinterBackend.testPrint(error)) {
    configServer.send(503, "text/plain", String("Test print failed: ") + error + "\n");
    return;
  }
  configServer.send(200, "text/html", "<p>Test print data was accepted by the USB transfer layer.</p><p>Check the physical printer for the page.</p><p><a href='/'>Back</a></p>");
}

void handleScan() {
  int n = WiFi.scanNetworks(false, true);
  String html = "<html><head><meta name='viewport' content='width=device-width,initial-scale=1'></head><body><h1>Wi-Fi networks</h1><ul>";
  for (int i = 0; i < n; ++i) {
    String ssid = WiFi.SSID(i);
    if (ssid.isEmpty()) ssid = "(hidden)";
    html += "<li>" + esc(ssid) + " — " + String(WiFi.RSSI(i)) + " dBm — ch " + String(WiFi.channel(i)) + "</li>";
  }
  html += "</ul><a href='/'>Back</a></body></html>";
  WiFi.scanDelete();
  configServer.send(200, "text/html; charset=utf-8", html);
}

void handleHealth() {
  String body = "wifi=" + String(WiFi.status() == WL_CONNECTED ? "connected" : "not-connected") + "\n";
  body += "ip=" + WiFi.localIP().toString() + "\n";
  body += "ap=" + WiFi.softAPIP().toString() + "\n";
  body += "ipp=" + String(ippServer.running() ? "ready" : "offline") + "\n";
  body += "usb=" + String(usbPrinterBackend.online() ? "printer-ready" : "not-ready") + "\n";
  body += "usb_reason=" + usbPrinterBackend.statusReason() + "\n";
  body += "usb_selection=" + String(usbHost.automaticInterfaceSelection() ? "auto" : "manual") + "\n";
  if (usbHost.selectedInterface()) {
    body += "usb_interface=" + String(usbHost.selectedInterface()->interfaceNumber) + "\n";
    body += "usb_alt=" + String(usbHost.selectedInterface()->alternateSetting) + "\n";
    body += "usb_out=0x" + String(usbHost.selectedInterface()->bulkOut.address, HEX) + "\n";
  }
  body += "active_jobs=" + String(printQueue.activeCount()) + "\n";
  body += "retained_jobs=" + String(printQueue.count()) + "\n";
  configServer.send(200, "text/plain", body);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== HP Print Server / ESP32-S3 / Mobile-first ===");

  loadConfig();
  usbHost.setInterfaceSelection(config.usbAuto, config.usbInterface, config.usbAlt);

  if (!printQueue.begin()) Serial.println("[Queue] Persistent queue unavailable");
  if (!usbPrinterBackend.begin())
    Serial.println("[USB] Host start failed: " + usbPrinterBackend.statusReason());

  // Station mode is always preferred. The AP is only a fallback when there
  // are no credentials or the router cannot be reached.
  bool wifi = connectWiFi();
  if (!wifi) startConfigAP();

  // Start IPP on both STA and fallback AP. This makes direct AP printing
  // possible while still making the normal router path the preferred mode.
  ippServer.begin(config.printerName, printerUri(), handleMobileJob, &printQueue);
  advertiseMobilePrinter();

  configServer.on("/", HTTP_GET, handleRoot);
  configServer.on("/scan", HTTP_GET, handleScan);
  configServer.on("/health", HTTP_GET, handleHealth);
  configServer.on("/save", HTTP_POST, handleSave);
  configServer.on("/usb", HTTP_POST, handleUsbSave);
  configServer.on("/usb/test", HTTP_POST, handleUsbTestPrint);
  configServer.begin();

  Serial.println("[HTTP] Configuration server ready");
  if (wifi) {
    Serial.print("[HTTP] Open http://");
    Serial.print(WiFi.localIP());
    Serial.println("/");
  } else {
    Serial.print("[HTTP] Connect to ");
    Serial.print(AP_SSID);
    Serial.println(" and open http://192.168.4.1/");
  }
}

void loop() {
  configServer.handleClient();
  ippServer.poll();
  usbPrinterBackend.poll();

  const UsbPrinterInterfaceInfo *p = usbHost.selectedInterface();
  const bool rawReady = p && p->protocol == 0x02 && p->usableForRawPrint();
  if (usbPrinterBackend.online() && rawReady && printQueue.hasPending()) {
    String error;
    if (!usbPrinterBackend.processNext(printQueue, error))
      Serial.println("[PRINT] Job failed: " + error);
  }
  delay(1);
}
