#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include "usb_host_manager.h"

// One-flash Android/HP print probe.
// Safe mode is the default: IPP is fully negotiated and PCLm is extracted,
// but nothing is sent to USB until the dashboard toggle is enabled.

namespace {
constexpr const char *HOSTNAME = "printer";
constexpr const char *CONFIG_NS = "hp-print";
constexpr const char *PROBE_AP_SSID = "HP-Print-Probe";
constexpr const char *PROBE_AP_PASSWORD = "probe1234";
constexpr const char *MODEL = "HP Smart Tank 520_540 series";
constexpr uint16_t IPP_PORT = 631;
constexpr uint16_t RAW_PORT = 9100;
constexpr uint32_t CLIENT_TIMEOUT_MS = 180000;
constexpr size_t HTTP_HEADER_LIMIT = 4096;
constexpr size_t IPP_PREFIX_LIMIT = 16384;
constexpr size_t USB_CHUNK = 1024;
constexpr size_t PREVIEW_BYTES = 64;

Preferences prefs;
WebServer web(80);
WiFiServer ippServer(IPP_PORT);
WiFiServer rawServer(RAW_PORT);
UsbHostManager usbHost;

bool usbForwardingEnabled = false;
uint32_t jobSequence = 0;

struct LastJob {
  bool seen = false;
  uint32_t requestId = 0;
  uint32_t jobId = 0;
  String format;
  String source;
  uint64_t documentBytes = 0;
  uint32_t fnv1a = 2166136261u;
  uint8_t first[PREVIEW_BYTES] = {};
  size_t firstLen = 0;
  uint8_t tail[PREVIEW_BYTES] = {};
  size_t tailLen = 0;
  size_t tailPos = 0;
  bool chunked = false;
  bool usbAttempted = false;
  bool usbSuccess = false;
  String usbError;
};

LastJob lastJob;

String htmlEscape(String s) {
  s.replace("&", "&amp;");
  s.replace("<", "&lt;");
  s.replace(">", "&gt;");
  s.replace("\"", "&quot;");
  return s;
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

String usbStateText() {
  switch (usbHost.state()) {
    case UsbHostManager::STOPPED: return "Stopped";
    case UsbHostManager::RUNNING: return "Waiting for USB printer";
    case UsbHostManager::ENUMERATING: return "Enumerating";
    case UsbHostManager::DEVICE_ATTACHED: return "Device attached, no RAW print interface";
    case UsbHostManager::PRINTER_READY: return "Printer ready";
    case UsbHostManager::ERROR: return String("USB error: ") + usbHost.lastError();
  }
  return "Unknown";
}

bool readClientByte(WiFiClient &client, uint8_t &out, uint32_t timeoutMs = CLIENT_TIMEOUT_MS) {
  const uint32_t started = millis();
  while (client.connected() || client.available()) {
    if (client.available() > 0) {
      const int v = client.read();
      if (v >= 0) { out = (uint8_t)v; return true; }
    }
    if (millis() - started >= timeoutMs) return false;
    delay(1);
  }
  return false;
}

bool readHttpHeader(WiFiClient &client, String &header) {
  header = "";
  header.reserve(1024);
  uint8_t b = 0;
  uint8_t window[4] = {};
  size_t count = 0;
  while (header.length() < HTTP_HEADER_LIMIT) {
    if (!readClientByte(client, b, 10000)) return false;
    header += (char)b;
    window[count % 4] = b;
    ++count;
    if (count >= 4) {
      const uint8_t a = window[(count - 4) % 4];
      const uint8_t c = window[(count - 3) % 4];
      const uint8_t d = window[(count - 2) % 4];
      const uint8_t e = window[(count - 1) % 4];
      if (a == '\r' && c == '\n' && d == '\r' && e == '\n') return true;
    }
  }
  return false;
}

String headerValue(const String &header, const char *name) {
  String lower = header;
  lower.toLowerCase();
  String key = String(name); key.toLowerCase(); key += ":";
  const int at = lower.indexOf(key);
  if (at < 0) return "";
  int p = at + key.length();
  while (p < header.length() && (header[p] == ' ' || header[p] == '\t')) ++p;
  int end = header.indexOf("\r\n", p);
  if (end < 0) end = header.length();
  String value = header.substring(p, end);
  value.trim();
  return value;
}

struct HttpBodyReader {
  WiFiClient &client;
  bool chunked = false;
  int64_t remaining = -1;
  size_t chunkRemaining = 0;
  bool done = false;

  explicit HttpBodyReader(WiFiClient &c) : client(c) {}

  bool rawByte(uint8_t &b) { return readClientByte(client, b); }

  bool rawLine(String &line) {
    line = "";
    uint8_t b = 0;
    while (line.length() < 128) {
      if (!rawByte(b)) return false;
      if (b == '\n') {
        if (line.endsWith("\r")) line.remove(line.length() - 1);
        return true;
      }
      line += (char)b;
    }
    return false;
  }

  bool nextChunk() {
    String line;
    do {
      if (!rawLine(line)) return false;
    } while (line.length() == 0);
    const int semi = line.indexOf(';');
    if (semi >= 0) line = line.substring(0, semi);
    line.trim();
    char *endp = nullptr;
    const unsigned long n = strtoul(line.c_str(), &endp, 16);
    if (!endp || *endp != 0) return false;
    if (n == 0) {
      do {
        if (!rawLine(line)) break;
      } while (line.length() != 0);
      done = true;
      return false;
    }
    chunkRemaining = (size_t)n;
    return true;
  }

  bool readByte(uint8_t &b) {
    if (done) return false;
    if (!chunked) {
      if (remaining == 0) { done = true; return false; }
      if (!rawByte(b)) return false;
      if (remaining > 0) --remaining;
      return true;
    }

    if (chunkRemaining == 0 && !nextChunk()) return false;
    if (!rawByte(b)) return false;
    --chunkRemaining;
    if (chunkRemaining == 0) {
      uint8_t cr = 0, lf = 0;
      if (!rawByte(cr) || !rawByte(lf) || cr != '\r' || lf != '\n') return false;
    }
    return true;
  }

  bool readExact(uint8_t *dst, size_t n) {
    for (size_t i = 0; i < n; ++i) if (!readByte(dst[i])) return false;
    return true;
  }
};

struct IppWriter {
  uint8_t data[4096];
  size_t len = 0;
  bool ok = true;

  void b(uint8_t v) { if (len < sizeof(data)) data[len++] = v; else ok = false; }
  void u16(uint16_t v) { b((uint8_t)(v >> 8)); b((uint8_t)v); }
  void u32(uint32_t v) { b((uint8_t)(v >> 24)); b((uint8_t)(v >> 16)); b((uint8_t)(v >> 8)); b((uint8_t)v); }
  void raw(const uint8_t *p, size_t n) {
    if (!ok || len + n > sizeof(data)) { ok = false; return; }
    memcpy(data + len, p, n); len += n;
  }
  void attr(uint8_t tag, const char *name, const uint8_t *value, uint16_t valueLen) {
    const uint16_t nameLen = name ? (uint16_t)strlen(name) : 0;
    b(tag); u16(nameLen); if (nameLen) raw((const uint8_t *)name, nameLen);
    u16(valueLen); if (valueLen) raw(value, valueLen);
  }
  void str(uint8_t tag, const char *name, const char *value) {
    attr(tag, name, (const uint8_t *)value, (uint16_t)strlen(value));
  }
  void integer(uint8_t tag, const char *name, int32_t value) {
    uint8_t v[4] = {(uint8_t)(value >> 24), (uint8_t)(value >> 16), (uint8_t)(value >> 8), (uint8_t)value};
    attr(tag, name, v, 4);
  }
  void boolean(const char *name, bool value) { const uint8_t v = value ? 1 : 0; attr(0x22, name, &v, 1); }
  void resolution(const char *name, int32_t x, int32_t y, uint8_t units) {
    uint8_t v[9] = {(uint8_t)(x >> 24),(uint8_t)(x >> 16),(uint8_t)(x >> 8),(uint8_t)x,
                    (uint8_t)(y >> 24),(uint8_t)(y >> 16),(uint8_t)(y >> 8),(uint8_t)y,units};
    attr(0x32, name, v, 9);
  }
};

void moreString(IppWriter &w, uint8_t tag, const char *v) { w.str(tag, nullptr, v); }
void moreInteger(IppWriter &w, uint8_t tag, int32_t v) { w.integer(tag, nullptr, v); }

void beginIppResponse(IppWriter &w, uint8_t major, uint8_t minor, uint16_t status, uint32_t requestId) {
  w.b(major ? major : 2); w.b(major ? minor : 0); w.u16(status); w.u32(requestId);
  w.b(0x01);
  w.str(0x47, "attributes-charset", "utf-8");
  w.str(0x48, "attributes-natural-language", "en");
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

void buildPrinterAttributes(IppWriter &w, uint8_t major, uint8_t minor, uint32_t requestId) {
  beginIppResponse(w, major, minor, 0x0000, requestId);
  w.b(0x04);
  w.str(0x45, "printer-uri-supported", "ipp://printer.local:631/ipp/print");
  w.str(0x42, "printer-name", MODEL);
  w.str(0x41, "printer-info", MODEL);
  w.str(0x42, "printer-make-and-model", MODEL);
  w.str(0x45, "printer-more-info", "http://printer.local/");
  w.str(0x45, "printer-icons", "http://printer.local/webApps/images/printer.png");
  w.str(0x45, "printer-uuid", "urn:uuid:3f045454-0520-0540-4554-000000000001");
  w.integer(0x23, "printer-state", 3);
  w.str(0x44, "printer-state-reasons", "none");
  w.boolean("printer-is-accepting-jobs", true);
  w.integer(0x21, "queued-job-count", 0);
  w.str(0x44, "ipp-versions-supported", "1.1"); moreString(w, 0x44, "2.0");
  w.integer(0x23, "operations-supported", 0x0002); moreInteger(w, 0x23, 0x0004);
  moreInteger(w, 0x23, 0x0008); moreInteger(w, 0x23, 0x0009); moreInteger(w, 0x23, 0x000B);
  w.str(0x49, "document-format-supported", "application/PCLm");
  moreString(w, 0x49, "application/pdf"); moreString(w, 0x49, "image/jpeg");
  w.str(0x49, "document-format-default", "application/PCLm");
  w.boolean("color-supported", true);
  w.str(0x44, "print-color-mode-supported", "color"); moreString(w, 0x44, "monochrome");
  w.str(0x44, "print-color-mode-default", "color");
  w.str(0x44, "media-supported", "iso_a4_210x297mm"); moreString(w, 0x44, "na_letter_8.5x11in");
  w.str(0x44, "media-default", "iso_a4_210x297mm");
  w.str(0x44, "media-source-supported", "main"); w.str(0x44, "media-type-supported", "stationery");
  w.str(0x44, "sides-supported", "one-sided"); w.str(0x44, "sides-default", "one-sided");
  w.boolean("page-ranges-supported", true);
  w.integer(0x23, "print-quality-supported", 3); moreInteger(w, 0x23, 4); moreInteger(w, 0x23, 5);
  w.integer(0x23, "print-quality-default", 4);
  w.resolution("printer-resolution-supported", 300, 300, 3); w.resolution(nullptr, 600, 600, 3);
  w.resolution("printer-resolution-default", 300, 300, 3);
  w.str(0x44, "uri-authentication-supported", "none");
  w.str(0x44, "uri-security-supported", "none");
  w.b(0x03);
}

void sendSimpleSuccess(WiFiClient &client, uint8_t major, uint8_t minor, uint32_t requestId) {
  IppWriter w; beginIppResponse(w, major, minor, 0x0000, requestId); w.b(0x03); sendHttpIpp(client, w);
}

void sendJobResponse(WiFiClient &client, uint8_t major, uint8_t minor, uint32_t requestId,
                     uint32_t jobId, bool success) {
  IppWriter w;
  beginIppResponse(w, major, minor, success ? 0x0000 : 0x0500, requestId);
  if (success) {
    w.b(0x02);
    w.integer(0x21, "job-id", (int32_t)jobId);
    String uri = String("ipp://printer.local:631/ipp/print/job/") + jobId;
    w.str(0x45, "job-uri", uri.c_str());
    w.integer(0x23, "job-state", 9);
    w.str(0x44, "job-state-reasons", "job-completed-successfully");
  }
  w.b(0x03);
  sendHttpIpp(client, w);
}

bool readU16(HttpBodyReader &r, uint16_t &v) {
  uint8_t b[2]; if (!r.readExact(b, 2)) return false; v = ((uint16_t)b[0] << 8) | b[1]; return true;
}

bool parseIppAttributes(HttpBodyReader &r, String &documentFormat, String &jobName) {
  String currentName;
  while (true) {
    uint8_t tag = 0;
    if (!r.readByte(tag)) return false;
    if (tag == 0x03) return true;
    if (tag <= 0x0F) { currentName = ""; continue; }

    uint16_t nameLen = 0, valueLen = 0;
    if (!readU16(r, nameLen)) return false;
    String name;
    for (uint16_t i = 0; i < nameLen; ++i) { uint8_t b = 0; if (!r.readByte(b)) return false; name += (char)b; }
    if (nameLen) currentName = name;
    if (!readU16(r, valueLen)) return false;

    String value;
    const bool keep = currentName == "document-format" || currentName == "job-name";
    if (keep) value.reserve(valueLen);
    for (uint16_t i = 0; i < valueLen; ++i) {
      uint8_t b = 0; if (!r.readByte(b)) return false;
      if (keep) value += (char)b;
    }
    if (currentName == "document-format") documentFormat = value;
    else if (currentName == "job-name") jobName = value;
  }
}

void resetLastJob(uint32_t requestId, bool chunked) {
  lastJob = LastJob{};
  lastJob.seen = true;
  lastJob.requestId = requestId;
  lastJob.jobId = ++jobSequence;
  lastJob.chunked = chunked;
}

void noteDocumentBytes(const uint8_t *data, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    const uint8_t b = data[i];
    if (lastJob.firstLen < PREVIEW_BYTES) lastJob.first[lastJob.firstLen++] = b;
    if (lastJob.tailLen < PREVIEW_BYTES) lastJob.tail[lastJob.tailLen++] = b;
    else { lastJob.tail[lastJob.tailPos] = b; lastJob.tailPos = (lastJob.tailPos + 1) % PREVIEW_BYTES; }
    lastJob.fnv1a ^= b;
    lastJob.fnv1a *= 16777619u;
  }
  lastJob.documentBytes += n;
}

bool forwardUsb(const uint8_t *data, size_t n, String &error) {
  size_t offset = 0;
  while (offset < n) {
    const size_t part = min(USB_CHUNK, n - offset);
    size_t accepted = 0;
    if (!usbHost.bulkWrite(data + offset, part, accepted, 30000, error)) return false;
    if (accepted != part) { error = "USB short write"; return false; }
    offset += part;
  }
  return true;
}

bool consumeDocument(HttpBodyReader &r) {
  uint8_t buf[4096];
  bool usbOk = true;
  lastJob.usbAttempted = usbForwardingEnabled;
  lastJob.usbSuccess = false;

  if (usbForwardingEnabled && usbHost.state() != UsbHostManager::PRINTER_READY) {
    lastJob.usbError = "USB forwarding enabled but printer interface is not ready";
    usbOk = false;
  }

  while (true) {
    size_t n = 0;
    while (n < sizeof(buf)) {
      uint8_t b = 0;
      if (!r.readByte(b)) break;
      buf[n++] = b;
    }
    if (n == 0) break;
    noteDocumentBytes(buf, n);
    if (usbForwardingEnabled && usbOk) {
      String error;
      if (!forwardUsb(buf, n, error)) {
        usbOk = false;
        lastJob.usbError = error;
        Serial.printf("[PROBE][USB] Forwarding failed after %llu document bytes: %s\n",
                      (unsigned long long)lastJob.documentBytes, error.c_str());
      }
    }
  }

  if (usbForwardingEnabled && usbOk) {
    lastJob.usbSuccess = true;
    Serial.printf("[PROBE][USB] Forwarded %llu PCLm bytes to classic USB print interface\n",
                  (unsigned long long)lastJob.documentBytes);
  }
  return !usbForwardingEnabled || usbOk;
}

void printPreview(const uint8_t *data, size_t n, const char *label) {
  Serial.printf("[PROBE][JOB] %s (%u bytes): ", label, (unsigned)n);
  for (size_t i = 0; i < n; ++i) Serial.printf("%02X", data[i]);
  Serial.println();
}

void printLastJobSummary() {
  Serial.printf("[PROBE][JOB] id=%lu request-id=%lu format=%s transfer=%s bytes=%llu fnv1a=0x%08lX mode=%s\n",
                (unsigned long)lastJob.jobId, (unsigned long)lastJob.requestId,
                lastJob.format.c_str(), lastJob.chunked ? "chunked" : "content-length",
                (unsigned long long)lastJob.documentBytes, (unsigned long)lastJob.fnv1a,
                usbForwardingEnabled ? "USB-FORWARD" : "SAFE-CAPTURE");
  printPreview(lastJob.first, lastJob.firstLen, "first");
  uint8_t orderedTail[PREVIEW_BYTES] = {};
  size_t tailN = lastJob.tailLen;
  if (tailN < PREVIEW_BYTES) memcpy(orderedTail, lastJob.tail, tailN);
  else for (size_t i = 0; i < PREVIEW_BYTES; ++i) orderedTail[i] = lastJob.tail[(lastJob.tailPos + i) % PREVIEW_BYTES];
  printPreview(orderedTail, tailN, "tail");
  if (lastJob.firstLen >= 8) {
    String ascii;
    for (size_t i = 0; i < min(lastJob.firstLen, (size_t)32); ++i) {
      const uint8_t b = lastJob.first[i]; ascii += (b >= 32 && b <= 126) ? (char)b : '.';
    }
    Serial.printf("[PROBE][JOB] first ASCII: %s\n", ascii.c_str());
  }
}

void handleIppClient(WiFiClient client) {
  client.setNoDelay(true);
  client.setTimeout(CLIENT_TIMEOUT_MS);
  const IPAddress remote = client.remoteIP();
  Serial.printf("[PROBE][IPP] connection from %s:%u\n", remote.toString().c_str(), client.remotePort());

  String header;
  if (!readHttpHeader(client, header)) {
    Serial.println("[PROBE][IPP] failed to read HTTP header");
    client.stop(); return;
  }

  String firstLine = header.substring(0, header.indexOf("\r\n"));
  const String expect = headerValue(header, "Expect");
  const String transfer = headerValue(header, "Transfer-Encoding");
  const String lengthText = headerValue(header, "Content-Length");
  const bool chunked = transfer.equalsIgnoreCase("chunked");
  Serial.printf("[PROBE][IPP] %s transfer=%s content-length=%s\n",
                firstLine.c_str(), chunked ? "chunked" : "fixed", lengthText.c_str());

  if (expect.equalsIgnoreCase("100-continue")) {
    client.print("HTTP/1.1 100 Continue\r\n\r\n"); client.flush();
    Serial.println("[PROBE][IPP] TX HTTP 100 Continue");
  }

  HttpBodyReader body(client);
  body.chunked = chunked;
  body.remaining = chunked ? -1 : (lengthText.length() ? lengthText.toInt() : -1);

  uint8_t ippHeader[8];
  if (!body.readExact(ippHeader, sizeof(ippHeader))) {
    client.print("HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
    client.stop(); return;
  }

  const uint8_t major = ippHeader[0], minor = ippHeader[1];
  const uint16_t op = ((uint16_t)ippHeader[2] << 8) | ippHeader[3];
  const uint32_t requestId = ((uint32_t)ippHeader[4] << 24) | ((uint32_t)ippHeader[5] << 16) |
                             ((uint32_t)ippHeader[6] << 8) | ippHeader[7];
  Serial.printf("[PROBE][IPP] RX IPP %u.%u operation=0x%04X (%s) request-id=%lu\n",
                major, minor, op, ippOperationName(op), (unsigned long)requestId);

  String format, jobName;
  if (!parseIppAttributes(body, format, jobName)) {
    Serial.println("[PROBE][IPP] malformed/truncated IPP attributes");
    client.print("HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
    client.stop(); return;
  }

  if (op == 0x000B) {
    IppWriter w; buildPrinterAttributes(w, major, minor, requestId); sendHttpIpp(client, w);
    Serial.printf("[PROBE][IPP] TX successful-ok Get-Printer-Attributes request-id=%lu\n", (unsigned long)requestId);
  } else if (op == 0x0004) {
    Serial.printf("[PROBE][IPP] Validate-Job format=%s job=%s -> successful-ok\n", format.c_str(), jobName.c_str());
    sendSimpleSuccess(client, major, minor, requestId);
  } else if (op == 0x0002) {
    resetLastJob(requestId, chunked);
    lastJob.format = format;
    lastJob.source = jobName;
    Serial.printf("[PROBE][IPP] Print-Job format=%s job=%s; extracting document (%s)\n",
                  format.c_str(), jobName.c_str(), usbForwardingEnabled ? "USB forwarding ON" : "safe capture only");
    const bool ok = consumeDocument(body);
    printLastJobSummary();
    sendJobResponse(client, major, minor, requestId, lastJob.jobId, ok);
  } else if (op == 0x0009) {
    const uint32_t id = lastJob.jobId ? lastJob.jobId : 1;
    sendJobResponse(client, major, minor, requestId, id, true);
  } else if (op == 0x0008) {
    sendSimpleSuccess(client, major, minor, requestId);
  } else {
    IppWriter w; beginIppResponse(w, major, minor, 0x0501, requestId); w.b(0x03); sendHttpIpp(client, w);
    Serial.printf("[PROBE][IPP] operation 0x%04X unsupported\n", op);
  }

  client.stop();
}

void serviceIpp() {
  WiFiClient client = ippServer.available();
  if (client) handleIppClient(client);
}

void serviceRaw() {
  WiFiClient client = rawServer.available();
  if (!client) return;
  Serial.printf("[PROBE][RAW] :9100 connection from %s:%u\n", client.remoteIP().toString().c_str(), client.remotePort());
  uint8_t first[256] = {}; size_t firstLen = 0; uint64_t total = 0;
  uint32_t last = millis();
  while (client.connected() || client.available()) {
    while (client.available()) {
      uint8_t b = (uint8_t)client.read();
      if (firstLen < sizeof(first)) first[firstLen++] = b;
      ++total; last = millis();
    }
    if (millis() - last > 1000) break;
    delay(1);
  }
  Serial.printf("[PROBE][RAW] closed after %llu bytes; first=%u bytes captured\n",
                (unsigned long long)total, (unsigned)firstLen);
  client.stop();
}

String hexString(const uint8_t *data, size_t n) {
  String s; s.reserve(n * 2 + 1); char b[3];
  for (size_t i = 0; i < n; ++i) { snprintf(b, sizeof(b), "%02X", data[i]); s += b; }
  return s;
}

String jobSummaryHtml() {
  if (!lastJob.seen) return "<p>No Print-Job captured yet.</p>";
  String h;
  h += "<table>";
  h += "<tr><th>Job</th><td>" + String(lastJob.jobId) + "</td></tr>";
  h += "<tr><th>Format</th><td>" + htmlEscape(lastJob.format) + "</td></tr>";
  h += "<tr><th>Transfer</th><td>" + String(lastJob.chunked ? "chunked" : "content-length") + "</td></tr>";
  h += "<tr><th>Document bytes</th><td>" + String((unsigned long long)lastJob.documentBytes) + "</td></tr>";
  h += "<tr><th>FNV-1a</th><td>0x" + String(lastJob.fnv1a, HEX) + "</td></tr>";
  h += "<tr><th>First bytes</th><td><code>" + hexString(lastJob.first, lastJob.firstLen) + "</code></td></tr>";
  if (lastJob.usbAttempted) {
    h += "<tr><th>USB result</th><td>" + String(lastJob.usbSuccess ? "Forwarded successfully" : "FAILED: ") +
         htmlEscape(lastJob.usbError) + "</td></tr>";
  }
  h += "</table>";
  return h;
}

void handleWebRoot() {
  String html;
  html.reserve(12000);
  html += "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>Android Print Probe</title><style>body{font-family:system-ui;max-width:980px;margin:24px auto;padding:0 14px;color:#263238}section{border:1px solid #ddd;border-radius:12px;padding:16px;margin:12px 0}button{padding:10px 14px;border-radius:8px;border:1px solid #999;font-weight:600}button.danger{background:#b42318;color:white}table{border-collapse:collapse;width:100%}th,td{text-align:left;padding:7px;border-bottom:1px solid #eee}th{width:190px}code{word-break:break-all}.warn{background:#fff4e5;border:1px solid #f0c36d;padding:10px;border-radius:8px}</style></head><body>";
  html += "<h1>Android Print Probe — one flash</h1>";
  html += "<section><h2>Mode</h2>";
  html += String("<p><b>") + (usbForwardingEnabled ? "USB FORWARDING ENABLED" : "SAFE CAPTURE ONLY") + "</b></p>";
  if (!usbForwardingEnabled) {
    html += "<p class='warn'>Safe mode extracts and verifies PCLm but never sends it to the printer.</p>";
    html += "<form method='POST' action='/forward'><input type='hidden' name='enable' value='1'><button class='danger'>Enable USB forwarding</button></form>";
  } else {
    html += "<p class='warn'>Print-Job document bytes will now be sent to the classic USB printer interface. This setting resets to OFF after reboot.</p>";
    html += "<form method='POST' action='/forward'><input type='hidden' name='enable' value='0'><button>Disable USB forwarding</button></form>";
  }
  html += "</section><section><h2>USB</h2><p>" + htmlEscape(usbStateText()) + "</p>";
  if (usbHost.device().attached) html += "<p>VID:PID " + String(usbHost.device().vid, HEX) + ":" + String(usbHost.device().pid, HEX) + " · " + htmlEscape(usbHost.device().product) + "</p>";
  html += "</section><section><h2>Last IPP print job</h2>" + jobSummaryHtml() + "</section>";
  html += "<section><h2>What this build handles</h2><p>Get-Printer-Attributes → Validate-Job → Print-Job, HTTP 100-continue, chunked transfer decoding, PCLm extraction, job response, and optional USB forwarding.</p></section>";
  html += "</body></html>";
  web.send(200, "text/html; charset=utf-8", html);
}

void handleForwardToggle() {
  if (!web.hasArg("enable")) { web.send(400, "text/plain", "Missing enable"); return; }
  usbForwardingEnabled = web.arg("enable") == "1";
  Serial.printf("[PROBE] USB forwarding %s from dashboard\n", usbForwardingEnabled ? "ENABLED" : "DISABLED");
  web.sendHeader("Location", "/"); web.send(303);
}

void handlePrinterIcon() {
  static const uint8_t png[] = {
    0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,0x00,0x00,0x00,0x0D,0x49,0x48,0x44,0x52,
    0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x08,0x06,0x00,0x00,0x00,0x1F,0x15,0xC4,
    0x89,0x00,0x00,0x00,0x0D,0x49,0x44,0x41,0x54,0x08,0xD7,0x63,0xF8,0xCF,0xC0,0xF0,
    0x1F,0x00,0x05,0x00,0x01,0xFF,0x89,0x99,0x3D,0x1D,0x00,0x00,0x00,0x00,0x49,0x45,
    0x4E,0x44,0xAE,0x42,0x60,0x82
  };
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
  Serial.printf("[WiFi] Started probe AP %s / %s at %s\n",
                PROBE_AP_SSID, PROBE_AP_PASSWORD, WiFi.softAPIP().toString().c_str());
}

void advertiseProbe() {
  MDNS.end();
  if (!MDNS.begin(HOSTNAME)) { Serial.println("[mDNS] Failed to start"); return; }
  MDNS.setInstanceName(MODEL);
  MDNS.addService("ipp", "tcp", IPP_PORT);
  MDNS.addServiceTxt("ipp", "tcp", "txtvers", "1");
  MDNS.addServiceTxt("ipp", "tcp", "qtotal", "1");
  MDNS.addServiceTxt("ipp", "tcp", "rp", "ipp/print");
  MDNS.addServiceTxt("ipp", "tcp", "ty", MODEL);
  MDNS.addServiceTxt("ipp", "tcp", "product", "(HP Smart Tank 520_540 series)");
  MDNS.addServiceTxt("ipp", "tcp", "note", "ESP32 one-flash Android print probe");
  MDNS.addServiceTxt("ipp", "tcp", "pdl", "application/PCLm,application/pdf,image/jpeg");
  MDNS.addService("pdl-datastream", "tcp", RAW_PORT);
  MDNS.addServiceTxt("pdl-datastream", "tcp", "txtvers", "1");
  MDNS.addServiceTxt("pdl-datastream", "tcp", "ty", MODEL);
  MDNS.addService("http", "tcp", 80);
}
} // namespace

void setup() {
  Serial.begin(115200); delay(500);
  Serial.println();
  Serial.println("=== ESP32-S3 Android Print Probe — one-flash build ===");
  Serial.println("[PROBE] Safe capture mode is ON by default; USB forwarding must be enabled from dashboard");

  if (!connectSavedWiFi()) startProbeAp();
  advertiseProbe();

  if (!usbHost.begin()) Serial.printf("[USB] Host start failed: %s\n", usbHost.lastError().c_str());
  else Serial.println("[USB] Host started; connect the HP printer whenever ready");

  ippServer.begin(); rawServer.begin(); ippServer.setNoDelay(true); rawServer.setNoDelay(true);
  web.on("/", HTTP_GET, handleWebRoot);
  web.on("/forward", HTTP_POST, handleForwardToggle);
  web.on("/webApps/images/printer.png", HTTP_GET, handlePrinterIcon);
  web.begin();

  const String ip = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  Serial.printf("[PROBE] Dashboard: http://%s/ (or http://printer.local/)\n", ip.c_str());
  Serial.println("[PROBE] First run: leave SAFE CAPTURE enabled and print once from HP Print Service.");
}

void loop() {
  web.handleClient();
  usbHost.poll();
  serviceIpp();
  serviceRaw();
  delay(1);
}
