#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>

// Standalone diagnostic firmware.
// It intentionally DOES NOT initialize USB or forward jobs to the printer.
// Its only purpose is to observe what Android print services try to do after
// discovering an HP-like network printer on the LAN.

namespace {
constexpr const char *HOSTNAME = "printer";
constexpr const char *CONFIG_NS = "hp-print";  // Reuse Wi-Fi saved by the main firmware.
constexpr const char *PROBE_AP_SSID = "HP-Print-Probe";
constexpr const char *PROBE_AP_PASSWORD = "probe1234";
constexpr const char *MODEL = "HP Smart Tank 520_540 series";
constexpr size_t CAPTURE_LIMIT = 2048;
constexpr uint32_t IPP_IDLE_FINISH_MS = 1000;
constexpr uint32_t RAW_IDLE_FINISH_MS = 750;

Preferences prefs;
WebServer web(80);
WiFiServer ippServer(631);
WiFiServer rawServer(9100);
WiFiClient ippClient;
WiFiClient rawClient;

struct Capture {
  const char *name = "";
  uint8_t bytes[CAPTURE_LIMIT] = {};
  size_t captured = 0;
  size_t total = 0;
  IPAddress remoteIp;
  uint16_t remotePort = 0;
  uint32_t startedMs = 0;
  uint32_t lastByteMs = 0;
  bool active = false;
  bool everSeen = false;
};

Capture ippCapture{"IPP :631"};
Capture rawCapture{"RAW :9100"};

String htmlEscape(String s) {
  s.replace("&", "&amp;");
  s.replace("<", "&lt;");
  s.replace(">", "&gt;");
  s.replace("\"", "&quot;");
  return s;
}

void clearCapture(Capture &c) {
  c.captured = 0;
  c.total = 0;
  c.remoteIp = IPAddress();
  c.remotePort = 0;
  c.startedMs = 0;
  c.lastByteMs = 0;
  c.active = false;
  c.everSeen = false;
  memset(c.bytes, 0, sizeof(c.bytes));
}

const char *ippOperationName(uint16_t op) {
  switch (op) {
    case 0x0002: return "Print-Job";
    case 0x0003: return "Print-URI";
    case 0x0004: return "Validate-Job";
    case 0x0005: return "Create-Job";
    case 0x0006: return "Send-Document";
    case 0x0007: return "Send-URI";
    case 0x0008: return "Cancel-Job";
    case 0x0009: return "Get-Job-Attributes";
    case 0x000A: return "Get-Jobs";
    case 0x000B: return "Get-Printer-Attributes";
    case 0x0010: return "Pause-Printer";
    case 0x0011: return "Resume-Printer";
    case 0x0012: return "Purge-Jobs";
    case 0x0034: return "Create-Printer-Subscriptions";
    case 0x0038: return "Get-Notifications";
    default: return "unknown/extension";
  }
}

int findHeaderEnd(const Capture &c) {
  if (c.captured < 4) return -1;
  for (size_t i = 0; i + 3 < c.captured; ++i) {
    if (c.bytes[i] == '\r' && c.bytes[i + 1] == '\n' &&
        c.bytes[i + 2] == '\r' && c.bytes[i + 3] == '\n') {
      return (int)i + 4;
    }
  }
  return -1;
}

int contentLengthFromHeader(const Capture &c, int headerEnd) {
  if (headerEnd <= 0) return -1;
  String header;
  header.reserve((size_t)headerEnd + 1);
  for (int i = 0; i < headerEnd; ++i) header += (char)c.bytes[i];
  String lower = header;
  lower.toLowerCase();
  const int at = lower.indexOf("content-length:");
  if (at < 0) return -1;
  int p = at + 15;
  while (p < header.length() && (header[p] == ' ' || header[p] == '\t')) ++p;
  return header.substring(p).toInt();
}

void printHexPreview(const Capture &c, size_t maxBytes = 256) {
  const size_t n = min(c.captured, maxBytes);
  Serial.printf("[PROBE] %s first %u captured bytes (HEX):\n", c.name, (unsigned)n);
  for (size_t i = 0; i < n; i += 16) {
    Serial.printf("  %04X  ", (unsigned)i);
    for (size_t j = 0; j < 16; ++j) {
      if (i + j < n) Serial.printf("%02X ", c.bytes[i + j]);
      else Serial.print("   ");
    }
    Serial.print(" ");
    for (size_t j = 0; j < 16 && i + j < n; ++j) {
      const uint8_t b = c.bytes[i + j];
      Serial.print((b >= 32 && b <= 126) ? (char)b : '.');
    }
    Serial.println();
  }
}

void describeIpp(const Capture &c) {
  const int headerEnd = findHeaderEnd(c);
  if (headerEnd < 0) {
    Serial.println("[PROBE][IPP] No complete HTTP header captured yet");
    return;
  }

  String firstLine;
  for (int i = 0; i < headerEnd; ++i) {
    if (c.bytes[i] == '\r' || c.bytes[i] == '\n') break;
    firstLine += (char)c.bytes[i];
  }
  Serial.printf("[PROBE][IPP] HTTP request: %s\n", firstLine.c_str());

  const size_t body = (size_t)headerEnd;
  if (c.captured < body + 8) {
    Serial.println("[PROBE][IPP] HTTP body is shorter than an IPP header");
    return;
  }

  const uint8_t major = c.bytes[body];
  const uint8_t minor = c.bytes[body + 1];
  const uint16_t op = ((uint16_t)c.bytes[body + 2] << 8) | c.bytes[body + 3];
  const uint32_t requestId = ((uint32_t)c.bytes[body + 4] << 24) |
                             ((uint32_t)c.bytes[body + 5] << 16) |
                             ((uint32_t)c.bytes[body + 6] << 8) |
                             c.bytes[body + 7];

  Serial.printf("[PROBE][IPP] IPP %u.%u operation=0x%04X (%s) request-id=%lu\n",
                major, minor, op, ippOperationName(op), (unsigned long)requestId);
}

void startCapture(Capture &c, WiFiClient &client) {
  c.captured = 0;
  c.total = 0;
  c.remoteIp = client.remoteIP();
  c.remotePort = client.remotePort();
  c.startedMs = millis();
  c.lastByteMs = c.startedMs;
  c.active = true;
  c.everSeen = true;
  memset(c.bytes, 0, sizeof(c.bytes));
  Serial.printf("[PROBE] %s connection from %s:%u\n",
                c.name, c.remoteIp.toString().c_str(), c.remotePort);
}

void finishCapture(Capture &c, WiFiClient &client, bool sendIppFailure) {
  if (!c.active) return;
  c.active = false;

  Serial.printf("[PROBE] %s finished: total=%u captured=%u from %s:%u\n",
                c.name, (unsigned)c.total, (unsigned)c.captured,
                c.remoteIp.toString().c_str(), c.remotePort);

  if (c.name == ippCapture.name) describeIpp(c);
  printHexPreview(c);

  if (sendIppFailure && client) {
    static const char body[] = "ESP32 diagnostic IPP probe: request captured; IPP response intentionally not implemented.\n";
    client.print("HTTP/1.1 501 Not Implemented\r\n");
    client.print("Content-Type: text/plain; charset=utf-8\r\n");
    client.print("Connection: close\r\n");
    client.printf("Content-Length: %u\r\n\r\n", (unsigned)(sizeof(body) - 1));
    client.write((const uint8_t *)body, sizeof(body) - 1);
    client.flush();
  }

  if (client) client.stop();
  client = WiFiClient();
}

void acceptIfNeeded(WiFiServer &server, WiFiClient &client, Capture &c) {
  if (client && (client.connected() || client.available())) return;
  WiFiClient incoming = server.available();
  if (!incoming) return;
  client = incoming;
  client.setNoDelay(true);
  startCapture(c, client);
}

void readAvailable(WiFiClient &client, Capture &c) {
  uint8_t temp[512];
  while (client && client.available() > 0) {
    const int want = min((int)sizeof(temp), client.available());
    const int got = client.read(temp, want);
    if (got <= 0) break;
    c.total += (size_t)got;
    c.lastByteMs = millis();
    const size_t room = CAPTURE_LIMIT - c.captured;
    const size_t copy = min(room, (size_t)got);
    if (copy) {
      memcpy(c.bytes + c.captured, temp, copy);
      c.captured += copy;
    }
  }
}

void serviceIpp() {
  acceptIfNeeded(ippServer, ippClient, ippCapture);
  if (!ippClient || !ippCapture.active) return;
  readAvailable(ippClient, ippCapture);

  const int headerEnd = findHeaderEnd(ippCapture);
  if (headerEnd >= 0) {
    const int bodyLength = contentLengthFromHeader(ippCapture, headerEnd);
    if (bodyLength >= 0 && ippCapture.total >= (size_t)headerEnd + (size_t)bodyLength) {
      finishCapture(ippCapture, ippClient, true);
      return;
    }
  }

  if ((!ippClient.connected() && ippClient.available() == 0) ||
      (ippCapture.total > 0 && millis() - ippCapture.lastByteMs >= IPP_IDLE_FINISH_MS)) {
    finishCapture(ippCapture, ippClient, true);
  }
}

void serviceRaw() {
  acceptIfNeeded(rawServer, rawClient, rawCapture);
  if (!rawClient || !rawCapture.active) return;
  readAvailable(rawClient, rawCapture);

  if ((!rawClient.connected() && rawClient.available() == 0) ||
      (rawCapture.total > 0 && millis() - rawCapture.lastByteMs >= RAW_IDLE_FINISH_MS)) {
    finishCapture(rawCapture, rawClient, false);
  }
}

String captureText(const Capture &c) {
  if (!c.everSeen) return "No connection captured yet.";

  String out;
  out.reserve(6000);
  out += String(c.name) + "\n";
  out += "remote: " + c.remoteIp.toString() + ":" + String(c.remotePort) + "\n";
  out += "state: " + String(c.active ? "capturing" : "complete") + "\n";
  out += "total bytes seen: " + String((unsigned)c.total) + "\n";
  out += "bytes retained: " + String((unsigned)c.captured) + "\n\n";

  const int headerEnd = (&c == &ippCapture) ? findHeaderEnd(c) : -1;
  if (headerEnd > 0) {
    out += "HTTP / IPP header:\n";
    for (int i = 0; i < headerEnd; ++i) {
      const char ch = (char)c.bytes[i];
      if (ch == '\r') continue;
      out += ch;
    }
    out += "\n";
  }

  out += "HEX + ASCII (first captured bytes):\n";
  const size_t n = min(c.captured, (size_t)768);
  char line[96];
  for (size_t i = 0; i < n; i += 16) {
    int used = snprintf(line, sizeof(line), "%04X  ", (unsigned)i);
    for (size_t j = 0; j < 16; ++j) {
      if (i + j < n) used += snprintf(line + used, sizeof(line) - used, "%02X ", c.bytes[i + j]);
      else used += snprintf(line + used, sizeof(line) - used, "   ");
    }
    used += snprintf(line + used, sizeof(line) - used, " ");
    for (size_t j = 0; j < 16 && i + j < n && used + 2 < (int)sizeof(line); ++j) {
      const uint8_t b = c.bytes[i + j];
      line[used++] = (b >= 32 && b <= 126) ? (char)b : '.';
    }
    line[used++] = '\n';
    line[used] = 0;
    out += line;
  }
  return out;
}

void handleWebRoot() {
  String html;
  html.reserve(15000);
  html += "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<meta http-equiv='refresh' content='2'><title>Android Print Probe</title>";
  html += "<style>body{font-family:system-ui;margin:20px;max-width:1000px}pre{white-space:pre-wrap;word-break:break-word;background:#f4f4f4;padding:12px;border-radius:8px}code{font-family:monospace}a{margin-right:12px}</style></head><body>";
  html += "<h1>Android Print Probe</h1>";
  html += "<p>This diagnostic firmware does not talk to USB. Open the Android print service, search/add the printer, then refresh this page.</p>";
  html += "<p><b>IP:</b> " + WiFi.localIP().toString() + " &nbsp; <b>Hostname:</b> printer.local</p>";
  html += "<p><a href='/clear'>Clear captures</a></p>";
  html += "<h2>IPP :631</h2><pre>" + htmlEscape(captureText(ippCapture)) + "</pre>";
  html += "<h2>RAW :9100</h2><pre>" + htmlEscape(captureText(rawCapture)) + "</pre>";
  html += "</body></html>";
  web.send(200, "text/html; charset=utf-8", html);
}

void handleClear() {
  if (ippClient) ippClient.stop();
  if (rawClient) rawClient.stop();
  ippClient = WiFiClient();
  rawClient = WiFiClient();
  clearCapture(ippCapture);
  clearCapture(rawCapture);
  web.sendHeader("Location", "/");
  web.send(303);
}

bool connectSavedWiFi() {
  String ssid;
  String password;
  if (prefs.begin(CONFIG_NS, true)) {
    ssid = prefs.getString("ssid", "");
    password = prefs.getString("pass", "");
    prefs.end();
  }
  if (ssid.isEmpty()) return false;

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.begin(ssid.c_str(), password.c_str());
  Serial.printf("[WiFi] Connecting to saved network %s", ssid.c_str());
  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) return false;
  Serial.printf("[WiFi] Connected: %s\n", WiFi.localIP().toString().c_str());
  return true;
}

void startProbeAp() {
  WiFi.mode(WIFI_AP);
  WiFi.setHostname(HOSTNAME);
  WiFi.softAP(PROBE_AP_SSID, PROBE_AP_PASSWORD);
  Serial.printf("[WiFi] No usable saved Wi-Fi. Started AP %s / %s at %s\n",
                PROBE_AP_SSID, PROBE_AP_PASSWORD, WiFi.softAPIP().toString().c_str());
}

void advertiseProbe() {
  MDNS.end();
  if (!MDNS.begin(HOSTNAME)) {
    Serial.println("[mDNS] Failed to start");
    return;
  }

  MDNS.setInstanceName(MODEL);

  MDNS.addService("ipp", "tcp", 631);
  MDNS.addServiceTxt("ipp", "tcp", "txtvers", "1");
  MDNS.addServiceTxt("ipp", "tcp", "qtotal", "1");
  MDNS.addServiceTxt("ipp", "tcp", "rp", "ipp/print");
  MDNS.addServiceTxt("ipp", "tcp", "ty", MODEL);
  MDNS.addServiceTxt("ipp", "tcp", "product", "(HP Smart Tank 520_540 series)");
  MDNS.addServiceTxt("ipp", "tcp", "note", "ESP32 Android print probe");
  MDNS.addServiceTxt("ipp", "tcp", "pdl", "application/octet-stream");

  MDNS.addService("pdl-datastream", "tcp", 9100);
  MDNS.addServiceTxt("pdl-datastream", "tcp", "txtvers", "1");
  MDNS.addServiceTxt("pdl-datastream", "tcp", "ty", MODEL);
  MDNS.addServiceTxt("pdl-datastream", "tcp", "note", "ESP32 Android print probe");

  MDNS.addService("http", "tcp", 80);
  Serial.println("[mDNS] Advertising _ipp._tcp :631 and _pdl-datastream._tcp :9100");
}
}  // namespace

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("=== ESP32-S3 Android Print Discovery Probe ===");
  Serial.println("[WARNING] Diagnostic only: USB/printer forwarding is disabled in this firmware");

  if (!connectSavedWiFi()) startProbeAp();
  advertiseProbe();

  ippServer.begin();
  rawServer.begin();
  ippServer.setNoDelay(true);
  rawServer.setNoDelay(true);

  web.on("/", HTTP_GET, handleWebRoot);
  web.on("/clear", HTTP_GET, handleClear);
  web.begin();

  const String ip = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  Serial.printf("[PROBE] Dashboard: http://%s/ (or http://printer.local/)\n", ip.c_str());
  Serial.println("[PROBE] Now open HP Print Service or Android Default Print Service and search for printers.");
}

void loop() {
  web.handleClient();
  serviceIpp();
  serviceRaw();
  delay(1);
}
