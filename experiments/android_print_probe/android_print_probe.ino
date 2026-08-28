#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>

// Standalone diagnostic firmware, phase 2.
// USB/printer forwarding is intentionally disabled. This probe now answers
// Get-Printer-Attributes with a real IPP response so Android/HP Print Service
// can advance to its next operation, while retaining the first bytes of every
// IPP/RAW request for analysis.

namespace {
constexpr const char *HOSTNAME = "printer";
constexpr const char *CONFIG_NS = "hp-print";
constexpr const char *PROBE_AP_SSID = "HP-Print-Probe";
constexpr const char *PROBE_AP_PASSWORD = "probe1234";
constexpr const char *MODEL = "HP Smart Tank 520_540 series";
constexpr size_t CAPTURE_LIMIT = 8192;
constexpr uint32_t IPP_IDLE_FINISH_MS = 1200;
constexpr uint32_t RAW_IDLE_FINISH_MS = 750;
constexpr size_t IPP_RESPONSE_LIMIT = 4096;

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
  bool continueSent = false;
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
  c.continueSent = false;
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
        c.bytes[i + 2] == '\r' && c.bytes[i + 3] == '\n') return (int)i + 4;
  }
  return -1;
}

String httpHeaderString(const Capture &c, int headerEnd) {
  String header;
  if (headerEnd <= 0) return header;
  header.reserve((size_t)headerEnd + 1);
  for (int i = 0; i < headerEnd; ++i) header += (char)c.bytes[i];
  return header;
}

int contentLengthFromHeader(const Capture &c, int headerEnd) {
  String header = httpHeaderString(c, headerEnd);
  if (header.isEmpty()) return -1;
  String lower = header;
  lower.toLowerCase();
  const int at = lower.indexOf("content-length:");
  if (at < 0) return -1;
  int p = at + 15;
  while (p < header.length() && (header[p] == ' ' || header[p] == '\t')) ++p;
  return header.substring(p).toInt();
}

bool expects100Continue(const Capture &c, int headerEnd) {
  String header = httpHeaderString(c, headerEnd);
  header.toLowerCase();
  return header.indexOf("expect: 100-continue") >= 0;
}

bool parseIppHeader(const Capture &c, uint8_t &major, uint8_t &minor,
                    uint16_t &operation, uint32_t &requestId, size_t &bodyOffset) {
  const int headerEnd = findHeaderEnd(c);
  if (headerEnd < 0 || c.captured < (size_t)headerEnd + 8) return false;
  bodyOffset = (size_t)headerEnd;
  major = c.bytes[bodyOffset];
  minor = c.bytes[bodyOffset + 1];
  operation = ((uint16_t)c.bytes[bodyOffset + 2] << 8) | c.bytes[bodyOffset + 3];
  requestId = ((uint32_t)c.bytes[bodyOffset + 4] << 24) |
              ((uint32_t)c.bytes[bodyOffset + 5] << 16) |
              ((uint32_t)c.bytes[bodyOffset + 6] << 8) |
              c.bytes[bodyOffset + 7];
  return true;
}

void printHexPreview(const Capture &c, size_t maxBytes = 512) {
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
    Serial.println("[PROBE][IPP] No complete HTTP header captured");
    return;
  }
  String firstLine;
  for (int i = 0; i < headerEnd; ++i) {
    if (c.bytes[i] == '\r' || c.bytes[i] == '\n') break;
    firstLine += (char)c.bytes[i];
  }
  Serial.printf("[PROBE][IPP] HTTP request: %s\n", firstLine.c_str());

  uint8_t major = 0, minor = 0;
  uint16_t op = 0;
  uint32_t requestId = 0;
  size_t bodyOffset = 0;
  if (!parseIppHeader(c, major, minor, op, requestId, bodyOffset)) {
    Serial.println("[PROBE][IPP] HTTP body is shorter than an IPP header");
    return;
  }
  Serial.printf("[PROBE][IPP] RX IPP %u.%u operation=0x%04X (%s) request-id=%lu\n",
                major, minor, op, ippOperationName(op), (unsigned long)requestId);
}

struct IppWriter {
  uint8_t data[IPP_RESPONSE_LIMIT];
  size_t len = 0;
  bool ok = true;

  void b(uint8_t v) {
    if (len >= sizeof(data)) { ok = false; return; }
    data[len++] = v;
  }
  void u16(uint16_t v) { b((uint8_t)(v >> 8)); b((uint8_t)v); }
  void u32(uint32_t v) { b((uint8_t)(v >> 24)); b((uint8_t)(v >> 16)); b((uint8_t)(v >> 8)); b((uint8_t)v); }
  void raw(const uint8_t *p, size_t n) {
    if (!ok || len + n > sizeof(data)) { ok = false; return; }
    memcpy(data + len, p, n); len += n;
  }
  void attr(uint8_t tag, const char *name, const uint8_t *value, uint16_t valueLen) {
    const uint16_t nameLen = name ? (uint16_t)strlen(name) : 0;
    b(tag); u16(nameLen);
    if (nameLen) raw((const uint8_t *)name, nameLen);
    u16(valueLen); if (valueLen) raw(value, valueLen);
  }
  void str(uint8_t tag, const char *name, const char *value) {
    attr(tag, name, (const uint8_t *)value, (uint16_t)strlen(value));
  }
  void boolean(const char *name, bool value) {
    const uint8_t v = value ? 1 : 0; attr(0x22, name, &v, 1);
  }
  void integer(uint8_t tag, const char *name, int32_t value) {
    uint8_t v[4] = {(uint8_t)(value >> 24), (uint8_t)(value >> 16), (uint8_t)(value >> 8), (uint8_t)value};
    attr(tag, name, v, 4);
  }
  void resolution(const char *name, int32_t x, int32_t y, uint8_t units) {
    uint8_t v[9] = {
      (uint8_t)(x >> 24), (uint8_t)(x >> 16), (uint8_t)(x >> 8), (uint8_t)x,
      (uint8_t)(y >> 24), (uint8_t)(y >> 16), (uint8_t)(y >> 8), (uint8_t)y,
      units
    };
    attr(0x32, name, v, 9);
  }
};

void addMoreString(IppWriter &w, uint8_t tag, const char *value) {
  w.str(tag, nullptr, value);
}

void addMoreInteger(IppWriter &w, uint8_t tag, int32_t value) {
  w.integer(tag, nullptr, value);
}

bool buildPrinterAttributes(uint8_t requestMajor, uint8_t requestMinor, uint32_t requestId,
                            IppWriter &w) {
  const uint8_t major = requestMajor >= 1 ? requestMajor : 2;
  const uint8_t minor = requestMajor >= 1 ? requestMinor : 0;
  w.b(major); w.b(minor); w.u16(0x0000); w.u32(requestId);  // successful-ok

  // operation-attributes-tag
  w.b(0x01);
  w.str(0x47, "attributes-charset", "utf-8");
  w.str(0x48, "attributes-natural-language", "en");

  // printer-attributes-tag
  w.b(0x04);
  w.str(0x45, "printer-uri-supported", "ipp://printer.local:631/ipp/print");
  w.str(0x42, "printer-name", MODEL);
  w.str(0x41, "printer-info", MODEL);
  w.str(0x42, "printer-make-and-model", MODEL);
  w.str(0x45, "printer-more-info", "http://printer.local/");
  w.str(0x45, "printer-icons", "http://printer.local/webApps/images/printer.png");
  w.str(0x45, "printer-uuid", "urn:uuid:3f045454-0520-0540-4554-000000000001");
  w.integer(0x23, "printer-state", 3);  // idle
  w.str(0x44, "printer-state-reasons", "none");
  w.boolean("printer-is-accepting-jobs", true);
  w.integer(0x21, "queued-job-count", 0);

  w.str(0x44, "ipp-versions-supported", "1.1");
  addMoreString(w, 0x44, "2.0");

  w.integer(0x23, "operations-supported", 0x0002); // Print-Job
  addMoreInteger(w, 0x23, 0x0004);                  // Validate-Job
  addMoreInteger(w, 0x23, 0x000B);                  // Get-Printer-Attributes

  // Broad diagnostic format set: phase 2 wants the client to reveal which
  // format it actually selects next. These do not claim USB-side support.
  w.str(0x49, "document-format-supported", "application/pdf");
  addMoreString(w, 0x49, "image/jpeg");
  addMoreString(w, 0x49, "application/PCLm");
  addMoreString(w, 0x49, "application/octet-stream");
  w.str(0x49, "document-format-default", "application/pdf");

  w.boolean("color-supported", true);
  w.str(0x44, "print-color-mode-supported", "color");
  addMoreString(w, 0x44, "monochrome");
  w.str(0x44, "print-color-mode-default", "color");

  w.str(0x44, "media-supported", "iso_a4_210x297mm");
  addMoreString(w, 0x44, "na_letter_8.5x11in");
  w.str(0x44, "media-default", "iso_a4_210x297mm");
  w.str(0x44, "media-source-supported", "main");
  w.str(0x44, "media-type-supported", "stationery");

  w.str(0x44, "sides-supported", "one-sided");
  w.str(0x44, "sides-default", "one-sided");
  w.boolean("page-ranges-supported", true);
  w.str(0x44, "print-scaling-supported", "auto");
  addMoreString(w, 0x44, "auto-fit");
  addMoreString(w, 0x44, "fill");
  addMoreString(w, 0x44, "fit");
  addMoreString(w, 0x44, "none");

  w.integer(0x23, "print-quality-supported", 3); // draft
  addMoreInteger(w, 0x23, 4);                    // normal
  addMoreInteger(w, 0x23, 5);                    // high
  w.integer(0x23, "print-quality-default", 4);

  w.resolution("printer-resolution-supported", 300, 300, 3); // dpi
  w.resolution(nullptr, 600, 600, 3);
  w.resolution("printer-resolution-default", 300, 300, 3);

  w.str(0x44, "uri-authentication-supported", "none");
  w.str(0x44, "uri-security-supported", "none");
  w.str(0x44, "which-jobs-supported", "completed");
  addMoreString(w, 0x44, "not-completed");

  w.b(0x03); // end-of-attributes-tag
  return w.ok;
}

void sendHttpIpp(WiFiClient &client, const IppWriter &w) {
  client.print("HTTP/1.1 200 OK\r\n");
  client.print("Content-Type: application/ipp\r\n");
  client.print("Cache-Control: no-cache\r\n");
  client.print("Connection: close\r\n");
  client.printf("Content-Length: %u\r\n\r\n", (unsigned)w.len);
  client.write(w.data, w.len);
  client.flush();
}

void sendIppUnsupported(WiFiClient &client, uint8_t major, uint8_t minor, uint32_t requestId) {
  IppWriter w;
  w.b(major ? major : 2); w.b(major ? minor : 0);
  w.u16(0x0501);  // server-error-operation-not-supported
  w.u32(requestId);
  w.b(0x01);
  w.str(0x47, "attributes-charset", "utf-8");
  w.str(0x48, "attributes-natural-language", "en");
  w.b(0x03);
  sendHttpIpp(client, w);
}

void respondToIpp(Capture &c, WiFiClient &client) {
  uint8_t major = 0, minor = 0;
  uint16_t op = 0;
  uint32_t requestId = 0;
  size_t bodyOffset = 0;
  if (!parseIppHeader(c, major, minor, op, requestId, bodyOffset)) {
    static const char msg[] = "Malformed IPP request\n";
    client.print("HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Type: text/plain\r\n");
    client.printf("Content-Length: %u\r\n\r\n", (unsigned)(sizeof(msg) - 1));
    client.write((const uint8_t *)msg, sizeof(msg) - 1);
    return;
  }

  if (op == 0x000B) {
    IppWriter w;
    if (buildPrinterAttributes(major, minor, requestId, w)) {
      Serial.printf("[PROBE][IPP] TX successful-ok Get-Printer-Attributes request-id=%lu bytes=%u\n",
                    (unsigned long)requestId, (unsigned)w.len);
      sendHttpIpp(client, w);
      return;
    }
    Serial.println("[PROBE][IPP] Internal response buffer overflow");
  } else {
    Serial.printf("[PROBE][IPP] NEXT operation observed: 0x%04X (%s), request-id=%lu; returning operation-not-supported\n",
                  op, ippOperationName(op), (unsigned long)requestId);
    sendIppUnsupported(client, major, minor, requestId);
    return;
  }

  client.print("HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
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
  c.continueSent = false;
  memset(c.bytes, 0, sizeof(c.bytes));
  Serial.printf("[PROBE] %s connection from %s:%u\n", c.name,
                c.remoteIp.toString().c_str(), c.remotePort);
}

void finishCapture(Capture &c, WiFiClient &client, bool isIpp) {
  if (!c.active) return;
  c.active = false;
  Serial.printf("[PROBE] %s finished: total=%u captured=%u from %s:%u\n",
                c.name, (unsigned)c.total, (unsigned)c.captured,
                c.remoteIp.toString().c_str(), c.remotePort);
  if (isIpp) describeIpp(c);
  printHexPreview(c);
  if (isIpp && client) respondToIpp(c, client);
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

void maybeSend100Continue() {
  if (!ippClient || !ippCapture.active || ippCapture.continueSent) return;
  const int headerEnd = findHeaderEnd(ippCapture);
  if (headerEnd < 0 || !expects100Continue(ippCapture, headerEnd)) return;
  ippClient.print("HTTP/1.1 100 Continue\r\n\r\n");
  ippClient.flush();
  ippCapture.continueSent = true;
  Serial.println("[PROBE][IPP] TX HTTP 100 Continue");
}

void serviceIpp() {
  acceptIfNeeded(ippServer, ippClient, ippCapture);
  if (!ippClient || !ippCapture.active) return;
  readAvailable(ippClient, ippCapture);
  maybeSend100Continue();

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
  out.reserve(9000);
  out += String(c.name) + "\n";
  out += "remote: " + c.remoteIp.toString() + ":" + String(c.remotePort) + "\n";
  out += "state: " + String(c.active ? "capturing" : "complete") + "\n";
  out += "total bytes seen: " + String((unsigned)c.total) + "\n";
  out += "bytes retained: " + String((unsigned)c.captured) + " / " + String((unsigned)CAPTURE_LIMIT) + "\n\n";

  const int headerEnd = (&c == &ippCapture) ? findHeaderEnd(c) : -1;
  if (headerEnd > 0) {
    out += "HTTP / IPP header:\n";
    for (int i = 0; i < headerEnd; ++i) {
      const char ch = (char)c.bytes[i];
      if (ch != '\r') out += ch;
    }
    out += "\n";

    uint8_t major = 0, minor = 0; uint16_t op = 0; uint32_t id = 0; size_t body = 0;
    if (parseIppHeader(c, major, minor, op, id, body)) {
      out += "Decoded: IPP " + String(major) + "." + String(minor) +
             " operation=0x" + String(op, HEX) + " (" + ippOperationName(op) +
             ") request-id=" + String(id) + "\n\n";
    }
  }

  out += "HEX + ASCII preview:\n";
  const size_t n = min(c.captured, (size_t)1536);
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
    line[used++] = '\n'; line[used] = 0; out += line;
  }
  return out;
}

void handleWebRoot() {
  String html;
  html.reserve(22000);
  html += "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<meta http-equiv='refresh' content='2'><title>Android Print Probe Phase 2</title>";
  html += "<style>body{font-family:system-ui;margin:20px;max-width:1100px}pre{white-space:pre-wrap;word-break:break-word;background:#f4f4f4;padding:12px;border-radius:8px}a{margin-right:12px}</style></head><body>";
  html += "<h1>Android Print Probe — Phase 2</h1>";
  html += "<p>USB is disabled. Get-Printer-Attributes receives a real successful IPP response. Any next IPP operation and RAW 9100 traffic are captured.</p>";
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
  ippClient = WiFiClient(); rawClient = WiFiClient();
  clearCapture(ippCapture); clearCapture(rawCapture);
  web.sendHeader("Location", "/"); web.send(303);
}

void handlePrinterIcon() {
  // The HP app probed this path in phase 1. A 1x1 transparent PNG is enough
  // to stop the cosmetic 404 without pretending it is a real product image.
  static const uint8_t png[] = {
    0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,0x00,0x00,0x00,0x0D,0x49,0x48,0x44,0x52,
    0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x08,0x06,0x00,0x00,0x00,0x1F,0x15,0xC4,
    0x89,0x00,0x00,0x00,0x0D,0x49,0x44,0x41,0x54,0x08,0xD7,0x63,0xF8,0xCF,0xC0,0xF0,
    0x1F,0x00,0x05,0x00,0x01,0xFF,0x89,0x99,0x3D,0x1D,0x00,0x00,0x00,0x00,0x49,0x45,
    0x4E,0x44,0xAE,0x42,0x60,0x82
  };
  web.sendHeader("Cache-Control", "no-cache");
  web.send_P(200, "image/png", (const char *)png, sizeof(png));
}

bool connectSavedWiFi() {
  String ssid, password;
  if (prefs.begin(CONFIG_NS, true)) {
    ssid = prefs.getString("ssid", ""); password = prefs.getString("pass", ""); prefs.end();
  }
  if (ssid.isEmpty()) return false;
  WiFi.mode(WIFI_STA); WiFi.setHostname(HOSTNAME); WiFi.begin(ssid.c_str(), password.c_str());
  Serial.printf("[WiFi] Connecting to saved network %s", ssid.c_str());
  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) { delay(250); Serial.print('.'); }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) return false;
  Serial.printf("[WiFi] Connected: %s\n", WiFi.localIP().toString().c_str());
  return true;
}

void startProbeAp() {
  WiFi.mode(WIFI_AP); WiFi.setHostname(HOSTNAME); WiFi.softAP(PROBE_AP_SSID, PROBE_AP_PASSWORD);
  Serial.printf("[WiFi] No usable saved Wi-Fi. Started AP %s / %s at %s\n",
                PROBE_AP_SSID, PROBE_AP_PASSWORD, WiFi.softAPIP().toString().c_str());
}

void advertiseProbe() {
  MDNS.end();
  if (!MDNS.begin(HOSTNAME)) { Serial.println("[mDNS] Failed to start"); return; }
  MDNS.setInstanceName(MODEL);
  MDNS.addService("ipp", "tcp", 631);
  MDNS.addServiceTxt("ipp", "tcp", "txtvers", "1");
  MDNS.addServiceTxt("ipp", "tcp", "qtotal", "1");
  MDNS.addServiceTxt("ipp", "tcp", "rp", "ipp/print");
  MDNS.addServiceTxt("ipp", "tcp", "ty", MODEL);
  MDNS.addServiceTxt("ipp", "tcp", "product", "(HP Smart Tank 520_540 series)");
  MDNS.addServiceTxt("ipp", "tcp", "note", "ESP32 Android print probe phase 2");
  MDNS.addServiceTxt("ipp", "tcp", "pdl", "application/pdf,image/jpeg,application/PCLm,application/octet-stream");
  MDNS.addService("pdl-datastream", "tcp", 9100);
  MDNS.addServiceTxt("pdl-datastream", "tcp", "txtvers", "1");
  MDNS.addServiceTxt("pdl-datastream", "tcp", "ty", MODEL);
  MDNS.addServiceTxt("pdl-datastream", "tcp", "note", "ESP32 Android print probe phase 2");
  MDNS.addService("http", "tcp", 80);
  Serial.println("[mDNS] Advertising _ipp._tcp :631 and _pdl-datastream._tcp :9100");
}
}  // namespace

void setup() {
  Serial.begin(115200); delay(500);
  Serial.println();
  Serial.println("=== ESP32-S3 Android Print Discovery Probe — Phase 2 ===");
  Serial.println("[WARNING] Diagnostic only: USB/printer forwarding is disabled");
  Serial.println("[IPP] Get-Printer-Attributes will receive a real successful-ok response");

  if (!connectSavedWiFi()) startProbeAp();
  advertiseProbe();
  ippServer.begin(); rawServer.begin(); ippServer.setNoDelay(true); rawServer.setNoDelay(true);
  web.on("/", HTTP_GET, handleWebRoot);
  web.on("/clear", HTTP_GET, handleClear);
  web.on("/webApps/images/printer.png", HTTP_GET, handlePrinterIcon);
  web.begin();

  const String ip = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  Serial.printf("[PROBE] Dashboard: http://%s/ (or http://printer.local/)\n", ip.c_str());
  Serial.println("[PROBE] Open HP Print Service / Android print service and repeat the same add/print attempt.");
}

void loop() {
  web.handleClient();
  serviceIpp();
  serviceRaw();
  delay(1);
}
