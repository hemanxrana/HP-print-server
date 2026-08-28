#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>

// Android print probe v2.
// Diagnostic only: no USB initialization and no printer forwarding.
// It logs incoming IPP attributes, answers Get-Printer-Attributes with a real
// minimal IPP response, and accepts the next common job operations so we can
// observe which document format Android actually sends.

namespace {
constexpr const char *HOSTNAME = "printer";
constexpr const char *CONFIG_NS = "hp-print";
constexpr const char *PROBE_AP_SSID = "HP-Print-Probe";
constexpr const char *PROBE_AP_PASSWORD = "probe1234";
constexpr const char *MODEL = "HP Smart Tank 520_540 series";
constexpr size_t CAPTURE_LIMIT = 8192;
constexpr uint32_t IPP_IDLE_FINISH_MS = 1500;
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
  bool sentContinue = false;
};

Capture ippCapture{"IPP :631"};
Capture rawCapture{"RAW :9100"};
String lastIppSummary;

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
  c.sentContinue = false;
  memset(c.bytes, 0, sizeof(c.bytes));
}

const char *ippOperationName(uint16_t op) {
  switch (op) {
    case 0x0002: return "Print-Job";
    case 0x0004: return "Validate-Job";
    case 0x0005: return "Create-Job";
    case 0x0006: return "Send-Document";
    case 0x0008: return "Cancel-Job";
    case 0x0009: return "Get-Job-Attributes";
    case 0x000A: return "Get-Jobs";
    case 0x000B: return "Get-Printer-Attributes";
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

String httpHeaderText(const Capture &c, int headerEnd) {
  String s;
  if (headerEnd <= 0) return s;
  s.reserve(headerEnd + 1);
  for (int i = 0; i < headerEnd; ++i) s += (char)c.bytes[i];
  return s;
}

int contentLengthFromHeader(const Capture &c, int headerEnd) {
  String header = httpHeaderText(c, headerEnd);
  if (!header.length()) return -1;
  String lower = header;
  lower.toLowerCase();
  const int at = lower.indexOf("content-length:");
  if (at < 0) return -1;
  int p = at + 15;
  while (p < header.length() && (header[p] == ' ' || header[p] == '\t')) ++p;
  return header.substring(p).toInt();
}

bool headerHas100Continue(const Capture &c, int headerEnd) {
  String h = httpHeaderText(c, headerEnd);
  h.toLowerCase();
  return h.indexOf("expect: 100-continue") >= 0;
}

uint16_t be16(const uint8_t *p) {
  return ((uint16_t)p[0] << 8) | p[1];
}

uint32_t be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | p[3];
}

void append16(uint8_t *buf, size_t &n, uint16_t v) {
  buf[n++] = (uint8_t)(v >> 8);
  buf[n++] = (uint8_t)v;
}

void append32(uint8_t *buf, size_t &n, uint32_t v) {
  buf[n++] = (uint8_t)(v >> 24);
  buf[n++] = (uint8_t)(v >> 16);
  buf[n++] = (uint8_t)(v >> 8);
  buf[n++] = (uint8_t)v;
}

void appendAttrRaw(uint8_t *buf, size_t &n, uint8_t tag, const char *name,
                   const uint8_t *value, uint16_t valueLen) {
  buf[n++] = tag;
  const uint16_t nameLen = name ? (uint16_t)strlen(name) : 0;
  append16(buf, n, nameLen);
  if (nameLen) {
    memcpy(buf + n, name, nameLen);
    n += nameLen;
  }
  append16(buf, n, valueLen);
  if (valueLen) {
    memcpy(buf + n, value, valueLen);
    n += valueLen;
  }
}

void appendAttrString(uint8_t *buf, size_t &n, uint8_t tag, const char *name, const char *value) {
  appendAttrRaw(buf, n, tag, name, (const uint8_t *)value, (uint16_t)strlen(value));
}

void appendAttrEnum(uint8_t *buf, size_t &n, const char *name, uint32_t value) {
  uint8_t v[4] = {(uint8_t)(value >> 24), (uint8_t)(value >> 16), (uint8_t)(value >> 8), (uint8_t)value};
  appendAttrRaw(buf, n, 0x23, name, v, 4);
}

void appendAttrInteger(uint8_t *buf, size_t &n, const char *name, uint32_t value) {
  uint8_t v[4] = {(uint8_t)(value >> 24), (uint8_t)(value >> 16), (uint8_t)(value >> 8), (uint8_t)value};
  appendAttrRaw(buf, n, 0x21, name, v, 4);
}

void appendAttrBoolean(uint8_t *buf, size_t &n, const char *name, bool value) {
  uint8_t v = value ? 1 : 0;
  appendAttrRaw(buf, n, 0x22, name, &v, 1);
}

void appendAttrRange(uint8_t *buf, size_t &n, const char *name, uint32_t low, uint32_t high) {
  uint8_t v[8] = {
    (uint8_t)(low >> 24), (uint8_t)(low >> 16), (uint8_t)(low >> 8), (uint8_t)low,
    (uint8_t)(high >> 24), (uint8_t)(high >> 16), (uint8_t)(high >> 8), (uint8_t)high
  };
  appendAttrRaw(buf, n, 0x33, name, v, 8);
}

void appendAttrResolution(uint8_t *buf, size_t &n, const char *name, uint32_t x, uint32_t y, uint8_t units) {
  uint8_t v[9] = {
    (uint8_t)(x >> 24), (uint8_t)(x >> 16), (uint8_t)(x >> 8), (uint8_t)x,
    (uint8_t)(y >> 24), (uint8_t)(y >> 16), (uint8_t)(y >> 8), (uint8_t)y,
    units
  };
  appendAttrRaw(buf, n, 0x32, name, v, 9);
}

String valueToText(uint8_t tag, const uint8_t *v, uint16_t len) {
  if ((tag == 0x21 || tag == 0x23) && len == 4) return String((long)be32(v));
  if (tag == 0x22 && len == 1) return v[0] ? "true" : "false";
  String s;
  bool printable = true;
  for (uint16_t i = 0; i < len; ++i) {
    if (v[i] < 32 || v[i] > 126) { printable = false; break; }
  }
  if (printable) {
    for (uint16_t i = 0; i < len; ++i) s += (char)v[i];
    return s;
  }
  s = "0x";
  const uint16_t shown = min<uint16_t>(len, 24);
  char b[3];
  for (uint16_t i = 0; i < shown; ++i) {
    snprintf(b, sizeof(b), "%02X", v[i]);
    s += b;
  }
  if (shown < len) s += "...";
  return s;
}

size_t decodeIppAttributes(const Capture &c, int headerEnd, String &summary) {
  summary = "";
  if (headerEnd < 0 || c.captured < (size_t)headerEnd + 8) return 0;
  size_t p = (size_t)headerEnd + 8;
  String lastName;
  Serial.println("[PROBE][IPP] Attributes:");
  summary += "Attributes:\n";

  while (p < c.captured) {
    const uint8_t tag = c.bytes[p++];
    if (tag == 0x03) {
      Serial.println("  <end-of-attributes>");
      summary += "<end-of-attributes>\n";
      return p;
    }
    if (tag >= 0x01 && tag <= 0x05) {
      Serial.printf("  [group 0x%02X]\n", tag);
      summary += "[group 0x" + String(tag, HEX) + "]\n";
      lastName = "";
      continue;
    }
    if (p + 2 > c.captured) break;
    const uint16_t nameLen = be16(c.bytes + p); p += 2;
    if (p + nameLen + 2 > c.captured) break;
    String name;
    if (nameLen) {
      for (uint16_t i = 0; i < nameLen; ++i) name += (char)c.bytes[p + i];
      lastName = name;
    } else {
      name = lastName;
    }
    p += nameLen;
    const uint16_t valueLen = be16(c.bytes + p); p += 2;
    if (p + valueLen > c.captured) break;
    const String value = valueToText(tag, c.bytes + p, valueLen);
    Serial.printf("    tag=0x%02X %s = %s\n", tag, name.c_str(), value.c_str());
    summary += "0x" + String(tag, HEX) + " " + name + " = " + value + "\n";
    p += valueLen;
  }
  summary += "<capture ended before end-of-attributes>\n";
  return p;
}

void describeDocumentSignature(const Capture &c, size_t documentOffset) {
  if (!documentOffset || documentOffset >= c.captured) return;
  const uint8_t *p = c.bytes + documentOffset;
  const size_t n = c.captured - documentOffset;
  const char *kind = "unknown";
  if (n >= 5 && memcmp(p, "%PDF-", 5) == 0) kind = "PDF";
  else if (n >= 3 && p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF) kind = "JPEG";
  else if (n >= 4 && memcmp(p, "RaS2", 4) == 0) kind = "PWG Raster";
  else if (n >= 4 && memcmp(p, "RaS3", 4) == 0) kind = "CUPS Raster";
  else if (n >= 9 && memcmp(p, "\x1B%-12345X", 9) == 0) kind = "PJL/PCL-family";
  Serial.printf("[PROBE][IPP] Document payload begins at captured offset %u; signature=%s\n",
                (unsigned)documentOffset, kind);
  lastIppSummary += "Document offset: " + String((unsigned)documentOffset) + "\n";
  lastIppSummary += "Document signature: " + String(kind) + "\n";
}

bool parseIppHeader(const Capture &c, int headerEnd, uint8_t &major, uint8_t &minor,
                    uint16_t &op, uint32_t &requestId) {
  if (headerEnd < 0 || c.captured < (size_t)headerEnd + 8) return false;
  const size_t body = (size_t)headerEnd;
  major = c.bytes[body];
  minor = c.bytes[body + 1];
  op = be16(c.bytes + body + 2);
  requestId = be32(c.bytes + body + 4);
  return true;
}

void sendHttpIpp(WiFiClient &client, const uint8_t *body, size_t bodyLen) {
  client.print("HTTP/1.1 200 OK\r\n");
  client.print("Content-Type: application/ipp\r\n");
  client.print("Connection: close\r\n");
  client.printf("Content-Length: %u\r\n\r\n", (unsigned)bodyLen);
  client.write(body, bodyLen);
  client.flush();
}

void sendSimpleSuccess(WiFiClient &client, uint8_t major, uint8_t minor, uint32_t requestId,
                       bool includeJob) {
  uint8_t out[768];
  size_t n = 0;
  out[n++] = major; out[n++] = minor; append16(out, n, 0x0000); append32(out, n, requestId);
  out[n++] = 0x01;
  appendAttrString(out, n, 0x47, "attributes-charset", "utf-8");
  appendAttrString(out, n, 0x48, "attributes-natural-language", "en");
  if (includeJob) {
    out[n++] = 0x02;
    appendAttrInteger(out, n, "job-id", 1);
    String jobUri = "ipp://" + WiFi.localIP().toString() + ":631/ipp/print/job/1";
    appendAttrString(out, n, 0x45, "job-uri", jobUri.c_str());
    appendAttrEnum(out, n, "job-state", 9); // completed; diagnostic sink
    appendAttrString(out, n, 0x44, "job-state-reasons", "job-completed-successfully");
  }
  out[n++] = 0x03;
  sendHttpIpp(client, out, n);
}

void sendPrinterAttributes(WiFiClient &client, uint8_t major, uint8_t minor, uint32_t requestId) {
  uint8_t out[4096];
  size_t n = 0;
  out[n++] = major; out[n++] = minor; append16(out, n, 0x0000); append32(out, n, requestId);

  out[n++] = 0x01;
  appendAttrString(out, n, 0x47, "attributes-charset", "utf-8");
  appendAttrString(out, n, 0x48, "attributes-natural-language", "en");

  out[n++] = 0x04;
  String printerUri = "ipp://" + WiFi.localIP().toString() + ":631/ipp/print";
  appendAttrString(out, n, 0x45, "printer-uri-supported", printerUri.c_str());
  appendAttrString(out, n, 0x42, "printer-name", MODEL);
  appendAttrString(out, n, 0x41, "printer-info", "ESP32 Android IPP diagnostic printer");
  appendAttrString(out, n, 0x41, "printer-make-and-model", MODEL);
  appendAttrString(out, n, 0x45, "printer-more-info", (String("http://") + WiFi.localIP().toString() + "/").c_str());
  appendAttrEnum(out, n, "printer-state", 3);
  appendAttrString(out, n, 0x44, "printer-state-reasons", "none");
  appendAttrBoolean(out, n, "printer-is-accepting-jobs", true);
  appendAttrInteger(out, n, "queued-job-count", 0);

  appendAttrString(out, n, 0x44, "ipp-versions-supported", "1.1");
  appendAttrString(out, n, 0x44, nullptr, "2.0");
  appendAttrEnum(out, n, "operations-supported", 0x0002);
  appendAttrEnum(out, n, nullptr, 0x0004);
  appendAttrEnum(out, n, nullptr, 0x0005);
  appendAttrEnum(out, n, nullptr, 0x0006);
  appendAttrEnum(out, n, nullptr, 0x0008);
  appendAttrEnum(out, n, nullptr, 0x0009);
  appendAttrEnum(out, n, nullptr, 0x000A);
  appendAttrEnum(out, n, nullptr, 0x000B);

  appendAttrString(out, n, 0x47, "charset-configured", "utf-8");
  appendAttrString(out, n, 0x47, "charset-supported", "utf-8");
  appendAttrString(out, n, 0x48, "natural-language-configured", "en");
  appendAttrString(out, n, 0x48, "generated-natural-language-supported", "en");
  appendAttrString(out, n, 0x44, "compression-supported", "none");

  appendAttrString(out, n, 0x49, "document-format-default", "application/pdf");
  appendAttrString(out, n, 0x49, "document-format-supported", "application/pdf");
  appendAttrString(out, n, 0x49, nullptr, "image/jpeg");
  appendAttrString(out, n, 0x49, nullptr, "image/pwg-raster");

  appendAttrBoolean(out, n, "color-supported", true);
  appendAttrString(out, n, 0x44, "print-color-mode-default", "color");
  appendAttrString(out, n, 0x44, "print-color-mode-supported", "color");
  appendAttrString(out, n, 0x44, nullptr, "monochrome");
  appendAttrString(out, n, 0x44, "sides-default", "one-sided");
  appendAttrString(out, n, 0x44, "sides-supported", "one-sided");
  appendAttrInteger(out, n, "copies-default", 1);
  appendAttrRange(out, n, "copies-supported", 1, 99);
  appendAttrString(out, n, 0x44, "media-default", "iso_a4_210x297mm");
  appendAttrString(out, n, 0x44, "media-supported", "iso_a4_210x297mm");
  appendAttrString(out, n, 0x44, nullptr, "na_letter_8.5x11in");
  appendAttrResolution(out, n, "printer-resolution-default", 300, 300, 3);
  appendAttrResolution(out, n, "printer-resolution-supported", 300, 300, 3);
  appendAttrResolution(out, n, nullptr, 600, 600, 3);

  out[n++] = 0x03;
  Serial.printf("[PROBE][IPP] Sending successful Get-Printer-Attributes response (%u bytes)\n", (unsigned)n);
  sendHttpIpp(client, out, n);
}

void printHexPreview(const Capture &c, size_t maxBytes = 256) {
  const size_t n = min(c.captured, maxBytes);
  Serial.printf("[PROBE] %s first %u captured bytes (HEX):\n", c.name, (unsigned)n);
  for (size_t i = 0; i < n; i += 16) {
    Serial.printf("  %04X  ", (unsigned)i);
    for (size_t j = 0; j < 16; ++j) {
      if (i + j < n) Serial.printf("%02X ", c.bytes[i + j]); else Serial.print("   ");
    }
    Serial.print(" ");
    for (size_t j = 0; j < 16 && i + j < n; ++j) {
      const uint8_t b = c.bytes[i + j];
      Serial.print((b >= 32 && b <= 126) ? (char)b : '.');
    }
    Serial.println();
  }
}

void startCapture(Capture &c, WiFiClient &client) {
  c.captured = 0; c.total = 0; c.remoteIp = client.remoteIP(); c.remotePort = client.remotePort();
  c.startedMs = millis(); c.lastByteMs = c.startedMs; c.active = true; c.everSeen = true; c.sentContinue = false;
  memset(c.bytes, 0, sizeof(c.bytes));
  Serial.printf("[PROBE] %s connection from %s:%u\n", c.name, c.remoteIp.toString().c_str(), c.remotePort);
}

void finishIppCapture() {
  if (!ippCapture.active) return;
  ippCapture.active = false;
  Serial.printf("[PROBE] IPP :631 finished: total=%u captured=%u from %s:%u\n",
                (unsigned)ippCapture.total, (unsigned)ippCapture.captured,
                ippCapture.remoteIp.toString().c_str(), ippCapture.remotePort);

  const int headerEnd = findHeaderEnd(ippCapture);
  uint8_t major = 1, minor = 1; uint16_t op = 0; uint32_t requestId = 1;
  if (!parseIppHeader(ippCapture, headerEnd, major, minor, op, requestId)) {
    Serial.println("[PROBE][IPP] Could not parse IPP header");
    if (ippClient) ippClient.stop();
    ippClient = WiFiClient();
    return;
  }

  Serial.printf("[PROBE][IPP] IPP %u.%u operation=0x%04X (%s) request-id=%lu\n",
                major, minor, op, ippOperationName(op), (unsigned long)requestId);
  size_t documentOffset = decodeIppAttributes(ippCapture, headerEnd, lastIppSummary);
  lastIppSummary = String("IPP ") + major + "." + minor + " " + ippOperationName(op) +
                   " request-id=" + String((unsigned long)requestId) + "\n" + lastIppSummary;
  if (op == 0x0002 || op == 0x0006) describeDocumentSignature(ippCapture, documentOffset);
  printHexPreview(ippCapture);

  if (ippClient) {
    if (op == 0x000B) sendPrinterAttributes(ippClient, major, minor, requestId);
    else if (op == 0x0004) sendSimpleSuccess(ippClient, major, minor, requestId, false);
    else if (op == 0x0002 || op == 0x0005 || op == 0x0006) sendSimpleSuccess(ippClient, major, minor, requestId, true);
    else {
      uint8_t out[256]; size_t n = 0;
      out[n++] = major; out[n++] = minor; append16(out, n, 0x0501); append32(out, n, requestId);
      out[n++] = 0x01;
      appendAttrString(out, n, 0x47, "attributes-charset", "utf-8");
      appendAttrString(out, n, 0x48, "attributes-natural-language", "en");
      out[n++] = 0x03;
      sendHttpIpp(ippClient, out, n);
    }
    ippClient.stop();
  }
  ippClient = WiFiClient();
}

void acceptIfNeeded(WiFiServer &server, WiFiClient &client, Capture &c) {
  if (client && (client.connected() || client.available())) return;
  WiFiClient incoming = server.available();
  if (!incoming) return;
  client = incoming; client.setNoDelay(true); startCapture(c, client);
}

void readAvailable(WiFiClient &client, Capture &c) {
  uint8_t temp[1024];
  while (client && client.available() > 0) {
    const int want = min((int)sizeof(temp), client.available());
    const int got = client.read(temp, want);
    if (got <= 0) break;
    c.total += (size_t)got; c.lastByteMs = millis();
    const size_t room = CAPTURE_LIMIT - c.captured;
    const size_t copy = min(room, (size_t)got);
    if (copy) { memcpy(c.bytes + c.captured, temp, copy); c.captured += copy; }
  }
}

void serviceIpp() {
  acceptIfNeeded(ippServer, ippClient, ippCapture);
  if (!ippClient || !ippCapture.active) return;
  readAvailable(ippClient, ippCapture);

  const int headerEnd = findHeaderEnd(ippCapture);
  if (headerEnd >= 0 && !ippCapture.sentContinue && headerHas100Continue(ippCapture, headerEnd)) {
    ippClient.print("HTTP/1.1 100 Continue\r\n\r\n");
    ippClient.flush();
    ippCapture.sentContinue = true;
    Serial.println("[PROBE][HTTP] Sent 100 Continue");
  }
  if (headerEnd >= 0) {
    const int bodyLength = contentLengthFromHeader(ippCapture, headerEnd);
    if (bodyLength >= 0 && ippCapture.total >= (size_t)headerEnd + (size_t)bodyLength) {
      finishIppCapture();
      return;
    }
  }
  if ((!ippClient.connected() && ippClient.available() == 0) ||
      (ippCapture.total > 0 && millis() - ippCapture.lastByteMs >= IPP_IDLE_FINISH_MS)) finishIppCapture();
}

void finishRawCapture() {
  if (!rawCapture.active) return;
  rawCapture.active = false;
  Serial.printf("[PROBE] RAW :9100 finished: total=%u captured=%u from %s:%u\n",
                (unsigned)rawCapture.total, (unsigned)rawCapture.captured,
                rawCapture.remoteIp.toString().c_str(), rawCapture.remotePort);
  printHexPreview(rawCapture);
  if (rawClient) rawClient.stop();
  rawClient = WiFiClient();
}

void serviceRaw() {
  acceptIfNeeded(rawServer, rawClient, rawCapture);
  if (!rawClient || !rawCapture.active) return;
  readAvailable(rawClient, rawCapture);
  if ((!rawClient.connected() && rawClient.available() == 0) ||
      (rawCapture.total > 0 && millis() - rawCapture.lastByteMs >= RAW_IDLE_FINISH_MS)) finishRawCapture();
}

String captureText(const Capture &c) {
  if (!c.everSeen) return "No connection captured yet.";
  String out;
  out.reserve(5000);
  out += String(c.name) + "\nremote: " + c.remoteIp.toString() + ":" + String(c.remotePort) + "\n";
  out += "state: " + String(c.active ? "capturing" : "complete") + "\n";
  out += "total bytes seen: " + String((unsigned)c.total) + "\nbytes retained: " + String((unsigned)c.captured) + "\n";
  return out;
}

void handleWebRoot() {
  String html;
  html.reserve(16000);
  html += "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<meta http-equiv='refresh' content='2'><title>Android Print Probe v2</title>";
  html += "<style>body{font-family:system-ui;margin:20px;max-width:1000px}pre{white-space:pre-wrap;word-break:break-word;background:#f4f4f4;padding:12px;border-radius:8px}</style></head><body>";
  html += "<h1>Android Print Probe v2</h1><p>Real minimal IPP capability response; USB remains disabled.</p>";
  html += "<p><b>IP:</b> " + WiFi.localIP().toString() + " &nbsp; <b>Hostname:</b> printer.local</p>";
  html += "<p><a href='/clear'>Clear captures</a></p>";
  html += "<h2>Last decoded IPP request</h2><pre>" + htmlEscape(lastIppSummary.length() ? lastIppSummary : "None yet") + "</pre>";
  html += "<h2>IPP :631</h2><pre>" + htmlEscape(captureText(ippCapture)) + "</pre>";
  html += "<h2>RAW :9100</h2><pre>" + htmlEscape(captureText(rawCapture)) + "</pre></body></html>";
  web.send(200, "text/html; charset=utf-8", html);
}

void handleClear() {
  if (ippClient) ippClient.stop(); if (rawClient) rawClient.stop();
  ippClient = WiFiClient(); rawClient = WiFiClient();
  clearCapture(ippCapture); clearCapture(rawCapture); lastIppSummary = "";
  web.sendHeader("Location", "/"); web.send(303);
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
  Serial.printf("[WiFi] Started AP %s / %s at %s\n", PROBE_AP_SSID, PROBE_AP_PASSWORD, WiFi.softAPIP().toString().c_str());
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
  MDNS.addServiceTxt("ipp", "tcp", "note", "ESP32 Android print probe v2");
  MDNS.addServiceTxt("ipp", "tcp", "pdl", "application/pdf,image/jpeg,image/pwg-raster");
  MDNS.addService("pdl-datastream", "tcp", 9100);
  MDNS.addServiceTxt("pdl-datastream", "tcp", "txtvers", "1");
  MDNS.addServiceTxt("pdl-datastream", "tcp", "ty", MODEL);
  MDNS.addService("http", "tcp", 80);
  Serial.println("[mDNS] Advertising IPP :631 and RAW :9100");
}
} // namespace

void setup() {
  Serial.begin(115200); delay(500); Serial.println();
  Serial.println("=== ESP32-S3 Android Print Probe v2 ===");
  Serial.println("[WARNING] Diagnostic only: USB/printer forwarding disabled");
  if (!connectSavedWiFi()) startProbeAp();
  advertiseProbe();
  ippServer.begin(); rawServer.begin(); ippServer.setNoDelay(true); rawServer.setNoDelay(true);
  web.on("/", HTTP_GET, handleWebRoot); web.on("/clear", HTTP_GET, handleClear); web.begin();
  const String ip = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  Serial.printf("[PROBE] Dashboard: http://%s/ (or http://printer.local/)\n", ip.c_str());
  Serial.println("[PROBE] Search/add the printer, then attempt one small test print.");
}

void loop() {
  web.handleClient(); serviceIpp(); serviceRaw(); delay(1);
}
