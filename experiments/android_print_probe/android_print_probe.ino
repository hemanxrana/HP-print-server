#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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
constexpr size_t IPP_RESPONSE_BUFFER_SIZE = 4096;
constexpr size_t DOCUMENT_BUFFER_SIZE = 1024;
constexpr size_t LOOP_STACK_CONFIGURED_BYTES = 16 * 1024;

Preferences prefs;
WebServer web(80);
WiFiServer ippServer(IPP_PORT);
WiFiServer rawServer(RAW_PORT);
UsbHostManager usbHost;

static uint8_t ippResponseBuffer[IPP_RESPONSE_BUFFER_SIZE];
static uint8_t documentBuffer[DOCUMENT_BUFFER_SIZE];
static uint8_t rawNetBuffer[4096];
static uint8_t rawUsbInBuffer[1024];
static uint8_t ippLiveNetBuffer[1024];
static uint8_t ippLiveUsbInBuffer[1024];

bool ippLiveActive = false;
uint64_t ippLiveNetToUsb = 0;
uint64_t ippLiveUsbToNet = 0;
uint32_t ippLiveOutTransfers = 0;
uint32_t ippLiveInTransfers = 0;
String ippLiveLastResponse;
String ippLiveLastError;

bool rawBridgeActive = false;
uint64_t rawBridgeLastNetToUsb = 0;
uint64_t rawBridgeLastUsbToNet = 0;
String rawBridgeLastError;

enum ProbeMode : uint8_t {
  MODE_SAFE = 0,
  MODE_CLASSIC_RAW = 1,
  MODE_IPP_USB = 2,
  MODE_IPP_LIVE = 3
};
ProbeMode probeMode = MODE_SAFE;

enum AdvertProfile : uint8_t {
  ADV_HP_INKJET_BROAD = 0,
  ADV_PCL3GUI_ONLY,
  ADV_PCL3GUI_PREFERRED,
  ADV_PCLM_ONLY,
  ADV_URF_ONLY,
  ADV_PWG_ONLY,
  ADV_JPEG_ONLY,
  ADV_AUTOMATIC_ONLY,
  ADV_PDF_EXPERIMENTAL
};
AdvertProfile advertProfile = ADV_HP_INKJET_BROAD;
bool mdnsRefreshPending = false;
uint32_t jobSequence = 0;
size_t minLoopStackFree = (size_t)-1;

struct LastRequest {
  bool seen = false;
  uint16_t operation = 0;
  uint32_t requestId = 0;
  String transfer;
  String format;
  String jobName;
};
LastRequest lastRequest;

struct LastJob {
  bool seen = false;
  uint32_t requestId = 0;
  uint32_t jobId = 0;
  String format;
  String source;
  String transport;
  uint64_t documentBytes = 0;
  uint64_t usbAccepted = 0;
  uint32_t fnv1a = 2166136261u;
  uint8_t first[PREVIEW_BYTES] = {};
  size_t firstLen = 0;
  uint8_t tail[PREVIEW_BYTES] = {};
  size_t tailLen = 0;
  size_t tailPos = 0;
  bool chunked = false;
  bool bodyComplete = false;
  bool usbAttempted = false;
  bool usbSuccess = false;
  bool responseProxied = false;
  int8_t physicalResult = 0; // 0 unknown, 1 printed, -1 no output
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

const char *modeName() {
  switch (probeMode) {
    case MODE_SAFE: return "SAFE CAPTURE";
    case MODE_CLASSIC_RAW: return "CLASSIC USB RAW";
    case MODE_IPP_USB: return "IPP-OVER-USB REBUILT";
    case MODE_IPP_LIVE: return "LIVE IPP USB DUPLEX";
  }
  return "UNKNOWN";
}

const char *advertProfileName() {
  switch (advertProfile) {
    case ADV_HP_INKJET_BROAD: return "HP INKJET BROAD";
    case ADV_PCL3GUI_ONLY: return "PCL3GUI ONLY";
    case ADV_PCL3GUI_PREFERRED: return "PCL3GUI PREFERRED";
    case ADV_PCLM_ONLY: return "PCLM ONLY";
    case ADV_URF_ONLY: return "APPLE RASTER / URF ONLY";
    case ADV_PWG_ONLY: return "PWG RASTER ONLY";
    case ADV_JPEG_ONLY: return "JPEG ONLY";
    case ADV_AUTOMATIC_ONLY: return "AUTOMATIC / OCTET-STREAM ONLY";
    case ADV_PDF_EXPERIMENTAL: return "PDF EXPERIMENTAL";
  }
  return "UNKNOWN";
}

String advertisedPdlList() {
  switch (advertProfile) {
    case ADV_PCL3GUI_ONLY: return "application/vnd.hp-PCL";
    case ADV_PCL3GUI_PREFERRED: return "application/vnd.hp-PCL,application/octet-stream,application/PCLm,image/jpeg,image/urf,image/pwg-raster";
    case ADV_PCLM_ONLY: return "application/PCLm";
    case ADV_URF_ONLY: return "image/urf";
    case ADV_PWG_ONLY: return "image/pwg-raster";
    case ADV_JPEG_ONLY: return "image/jpeg";
    case ADV_AUTOMATIC_ONLY: return "application/octet-stream";
    case ADV_PDF_EXPERIMENTAL: return "application/pdf";
    case ADV_HP_INKJET_BROAD:
    default: return "application/vnd.hp-PCL,image/jpeg,image/urf,image/pwg-raster,application/PCLm,application/octet-stream";
  }
}

String advertisedVersionList() {
  switch (advertProfile) {
    case ADV_PCL3GUI_ONLY: return "PCL3GUI,PCL3,PJL,Automatic";
    case ADV_PCL3GUI_PREFERRED: return "PCL3GUI,PCL3,PJL,Automatic,PCLM,JPEG,AppleRaster,PWGRaster";
    case ADV_PCLM_ONLY: return "PCLM";
    case ADV_URF_ONLY: return "AppleRaster";
    case ADV_PWG_ONLY: return "PWGRaster";
    case ADV_JPEG_ONLY: return "JPEG";
    case ADV_AUTOMATIC_ONLY: return "Automatic";
    case ADV_PDF_EXPERIMENTAL: return "PDF";
    case ADV_HP_INKJET_BROAD:
    default: return "PCL3GUI,PCL3,PJL,Automatic,JPEG,AppleRaster,PWGRaster,PCLM";
  }
}

const char *advertisedDefaultFormat() {
  switch (advertProfile) {
    case ADV_PCL3GUI_ONLY:
    case ADV_PCL3GUI_PREFERRED: return "application/vnd.hp-PCL";
    case ADV_PCLM_ONLY: return "application/PCLm";
    case ADV_URF_ONLY: return "image/urf";
    case ADV_PWG_ONLY: return "image/pwg-raster";
    case ADV_JPEG_ONLY: return "image/jpeg";
    case ADV_PDF_EXPERIMENTAL: return "application/pdf";
    case ADV_AUTOMATIC_ONLY:
    case ADV_HP_INKJET_BROAD:
    default: return "application/octet-stream";
  }
}

size_t currentLoopStackFree() {
  return (size_t)uxTaskGetStackHighWaterMark(nullptr);
}

void noteStack(const char *where, bool logNow = false) {
  const size_t freeNow = currentLoopStackFree();
  if (freeNow < minLoopStackFree) minLoopStackFree = freeNow;
  if (logNow) {
    Serial.printf("[PROBE][MEM] %s loop-stack-free=%u min=%u heap=%u psram=%u\n",
                  where, (unsigned)freeNow, (unsigned)minLoopStackFree,
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());
  }
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
  uint8_t *data = ippResponseBuffer;
  size_t capacity = IPP_RESPONSE_BUFFER_SIZE;
  size_t len = 0;
  bool ok = true;

  void b(uint8_t v) { if (len < capacity) data[len++] = v; else ok = false; }
  void u16(uint16_t v) { b((uint8_t)(v >> 8)); b((uint8_t)v); }
  void u32(uint32_t v) { b((uint8_t)(v >> 24)); b((uint8_t)(v >> 16)); b((uint8_t)(v >> 8)); b((uint8_t)v); }
  void raw(const uint8_t *p, size_t n) {
    if (!ok || len + n > capacity) { ok = false; return; }
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

void addAdvertisedDocumentFormats(IppWriter &w) {
  auto add = [&](const char *name, bool first) {
    if (first) w.str(0x49, "document-format-supported", name);
    else moreString(w, 0x49, name);
  };

  bool first = true;
  auto put = [&](const char *name) { add(name, first); first = false; };
  switch (advertProfile) {
    case ADV_PCL3GUI_ONLY: put("application/vnd.hp-PCL"); break;
    case ADV_PCL3GUI_PREFERRED:
      put("application/vnd.hp-PCL"); put("application/octet-stream"); put("application/PCLm");
      put("image/jpeg"); put("image/urf"); put("image/pwg-raster"); break;
    case ADV_PCLM_ONLY: put("application/PCLm"); break;
    case ADV_URF_ONLY: put("image/urf"); break;
    case ADV_PWG_ONLY: put("image/pwg-raster"); break;
    case ADV_JPEG_ONLY: put("image/jpeg"); break;
    case ADV_AUTOMATIC_ONLY: put("application/octet-stream"); break;
    case ADV_PDF_EXPERIMENTAL: put("application/pdf"); break;
    case ADV_HP_INKJET_BROAD:
    default:
      put("application/vnd.hp-PCL"); put("image/jpeg"); put("image/urf");
      put("image/pwg-raster"); put("application/PCLm"); put("application/octet-stream"); break;
  }
  w.str(0x49, "document-format-default", advertisedDefaultFormat());

  const String versions = advertisedVersionList();
  int start = 0;
  bool firstVersion = true;
  while (start < versions.length()) {
    int comma = versions.indexOf(',', start);
    if (comma < 0) comma = versions.length();
    const String value = versions.substring(start, comma);
    if (firstVersion) w.str(0x41, "document-format-version-supported", value.c_str());
    else moreString(w, 0x41, value.c_str());
    firstVersion = false;
    start = comma + 1;
  }
}

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
  addAdvertisedDocumentFormats(w);
  w.str(0x44, "pdl-override-supported", "attempted");
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
    lastJob.usbAccepted += accepted;
    offset += part;
  }
  return true;
}

bool ippUsbWriteAll(const uint8_t *data, size_t n, String &error) {
  size_t offset = 0;
  while (offset < n) {
    const size_t part = min((size_t)USB_CHUNK, n - offset);
    size_t accepted = 0;
    if (!usbHost.ippBulkWrite(data + offset, part, accepted, 30000, error)) return false;
    if (accepted != part) { error = "IPP-over-USB short write"; return false; }
    lastJob.usbAccepted += accepted;
    offset += part;
  }
  return true;
}

bool ippUsbChunk(const uint8_t *data, size_t n, String &error) {
  char line[20];
  const int len = snprintf(line, sizeof(line), "%X\r\n", (unsigned)n);
  if (len <= 0 || !ippUsbWriteAll((const uint8_t *)line, (size_t)len, error)) return false;
  if (n && !ippUsbWriteAll(data, n, error)) return false;
  static const uint8_t crlf[] = {'\r','\n'};
  return ippUsbWriteAll(crlf, sizeof(crlf), error);
}

bool beginIppUsbRequest(uint8_t major, uint8_t minor, uint32_t requestId,
                        const String &format, const String &jobName, String &error) {
  const int8_t selected = usbHost.selectedIppInterfaceIndex();
  if (selected < 0 || !usbHost.ippInterfaceAt((uint8_t)selected)) {
    error = "No claimed IPP-over-USB interface";
    return false;
  }
  const String http =
      "POST /ipp/print HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: application/ipp\r\n"
      "Transfer-Encoding: chunked\r\n"
      "Connection: close\r\n\r\n";
  if (!ippUsbWriteAll((const uint8_t *)http.c_str(), http.length(), error)) return false;

  IppWriter w;
  w.b(major ? major : 2); w.b(major ? minor : 0); w.u16(0x0002); w.u32(requestId);
  w.b(0x01);
  w.str(0x47, "attributes-charset", "utf-8");
  w.str(0x48, "attributes-natural-language", "en");
  w.str(0x45, "printer-uri", "ipp://localhost/ipp/print");
  w.str(0x42, "requesting-user-name", "android");
  w.str(0x42, "job-name", jobName.length() ? jobName.c_str() : "Android Print Job");
  w.str(0x49, "document-format", format.length() ? format.c_str() : "application/PCLm");
  w.b(0x03);
  if (!w.ok) { error = "Unable to build IPP-over-USB request prefix"; return false; }
  return ippUsbChunk(w.data, w.len, error);
}

bool proxyIppUsbResponse(WiFiClient &networkClient, bool &anyResponse, String &error) {
  anyResponse = false;
  String header;
  header.reserve(1024);
  bool headerDone = false;
  int64_t contentLength = -1;
  size_t total = 0;
  size_t headerBytes = 0;
  const size_t responseLimit = 128 * 1024;

  while (total < responseLimit) {
    size_t got = 0;
    String readError;
    if (!usbHost.ippBulkRead(documentBuffer, DOCUMENT_BUFFER_SIZE, got, 5000, readError)) {
      if (anyResponse && headerDone && contentLength < 0) return true;
      error = readError;
      return false;
    }
    anyResponse = true;
    networkClient.write(documentBuffer, got);
    networkClient.flush();
    total += got;

    if (!headerDone && header.length() < HTTP_HEADER_LIMIT) {
      for (size_t i = 0; i < got && header.length() < HTTP_HEADER_LIMIT; ++i) header += (char)documentBuffer[i];
      const int end = header.indexOf("\r\n\r\n");
      if (end >= 0) {
        headerDone = true;
        headerBytes = (size_t)end + 4;
        const String cl = headerValue(header, "Content-Length");
        if (cl.length()) contentLength = cl.toInt();
      }
    }
    if (headerDone && contentLength >= 0 && total >= headerBytes + (size_t)contentLength) return true;
    delay(1);
  }
  error = "IPP-over-USB response exceeded 128 KiB limit";
  return false;
}

bool consumeDocument(HttpBodyReader &r, WiFiClient &networkClient,
                     uint8_t major, uint8_t minor, uint32_t requestId,
                     const String &format, const String &jobName,
                     bool &responseSent) {
  responseSent = false;
  bool transportOk = true;
  bool anyUsbResponse = false;
  lastJob.usbAttempted = probeMode != MODE_SAFE;
  lastJob.usbSuccess = false;
  lastJob.responseProxied = false;
  lastJob.transport = modeName();

  if (probeMode == MODE_CLASSIC_RAW && usbHost.state() != UsbHostManager::PRINTER_READY) {
    lastJob.usbError = "Classic RAW selected but printer interface is not ready";
    transportOk = false;
  }
  if (probeMode == MODE_IPP_USB) {
    String error;
    if (!beginIppUsbRequest(major, minor, requestId, format, jobName, error)) {
      lastJob.usbError = error;
      transportOk = false;
    }
  }

  uint64_t nextProgress = 256 * 1024ULL;
  while (true) {
    size_t n = 0;
    while (n < DOCUMENT_BUFFER_SIZE) {
      uint8_t b = 0;
      if (!r.readByte(b)) break;
      documentBuffer[n++] = b;
    }
    if (n == 0) break;
    noteDocumentBytes(documentBuffer, n);

    if (transportOk && probeMode == MODE_CLASSIC_RAW) {
      String error;
      if (!forwardUsb(documentBuffer, n, error)) {
        transportOk = false;
        lastJob.usbError = error;
      }
    } else if (transportOk && probeMode == MODE_IPP_USB) {
      String error;
      if (!ippUsbChunk(documentBuffer, n, error)) {
        transportOk = false;
        lastJob.usbError = error;
      }
    }

    if (lastJob.documentBytes >= nextProgress) {
      Serial.printf("[PROBE][JOB] streamed %llu document bytes mode=%s\n",
                    (unsigned long long)lastJob.documentBytes, modeName());
      noteStack("print-stream", true);
      nextProgress += 256 * 1024ULL;
    }
    delay(1);
  }

  lastJob.bodyComplete = r.done;
  if (!lastJob.bodyComplete && lastJob.usbError.isEmpty()) lastJob.usbError = "HTTP/IPP document body ended unexpectedly";

  if (probeMode == MODE_IPP_USB && transportOk && lastJob.bodyComplete) {
    String error;
    if (!ippUsbChunk(nullptr, 0, error)) {
      transportOk = false;
      lastJob.usbError = error;
    } else {
      const bool completeResponse = proxyIppUsbResponse(networkClient, anyUsbResponse, error);
      responseSent = anyUsbResponse;
      lastJob.responseProxied = anyUsbResponse;
      if (!completeResponse) {
        transportOk = false;
        lastJob.usbError = error;
      }
    }
  }

  if (probeMode == MODE_CLASSIC_RAW) lastJob.usbSuccess = transportOk && lastJob.bodyComplete;
  else if (probeMode == MODE_IPP_USB) lastJob.usbSuccess = transportOk && lastJob.bodyComplete && anyUsbResponse;

  return probeMode == MODE_SAFE ? lastJob.bodyComplete : lastJob.usbSuccess;
}

void printPreview(const uint8_t *data, size_t n, const char *label) {
  Serial.printf("[PROBE][JOB] %s (%u bytes): ", label, (unsigned)n);
  for (size_t i = 0; i < n; ++i) Serial.printf("%02X", data[i]);
  Serial.println();
}

void printLastJobSummary() {
  Serial.printf("[PROBE][JOB] id=%lu request-id=%lu format=%s transfer=%s bytes=%llu complete=%d mode=%s usb-accepted=%llu usb-success=%d\n",
                (unsigned long)lastJob.jobId, (unsigned long)lastJob.requestId,
                lastJob.format.c_str(), lastJob.chunked ? "chunked" : "content-length",
                (unsigned long long)lastJob.documentBytes, (int)lastJob.bodyComplete,
                lastJob.transport.c_str(), (unsigned long long)lastJob.usbAccepted,
                (int)lastJob.usbSuccess);
  printPreview(lastJob.first, lastJob.firstLen, "first");
  uint8_t orderedTail[PREVIEW_BYTES] = {};
  const size_t tailN = lastJob.tailLen;
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
  if (lastJob.usbError.length()) Serial.printf("[PROBE][USB] %s\n", lastJob.usbError.c_str());
  noteStack("print-end", true);
}

String normalizeLiveIppHeader(const String &header) {
  String out;
  out.reserve(header.length() + 64);
  int pos = 0;
  bool hostDone = false;
  bool connectionDone = false;
  while (pos < header.length()) {
    int end = header.indexOf("\r\n", pos);
    if (end < 0) break;
    String line = header.substring(pos, end);
    pos = end + 2;
    if (line.length() == 0) break;
    String lower = line;
    lower.toLowerCase();
    if (lower.startsWith("host:")) {
      out += "Host: localhost\r\n";
      hostDone = true;
    } else if (lower.startsWith("connection:")) {
      out += "Connection: close\r\n";
      connectionDone = true;
    } else {
      out += line;
      out += "\r\n";
    }
  }
  if (!hostDone) out += "Host: localhost\r\n";
  if (!connectionDone) out += "Connection: close\r\n";
  out += "\r\n";
  return out;
}

bool liveIppUsbWrite(const uint8_t *data, size_t length, String &error) {
  size_t offset = 0;
  while (offset < length) {
    const size_t part = min((size_t)USB_CHUNK, length - offset);
    size_t accepted = 0;
    if (!usbHost.ippBulkWrite(data + offset, part, accepted, 30000, error)) return false;
    if (accepted != part) {
      error = "live IPP USB short OUT write";
      return false;
    }
    offset += accepted;
    ippLiveNetToUsb += accepted;
    ++ippLiveOutTransfers;
  }
  return true;
}

void noteLiveIppResponseBytes(const uint8_t *data, size_t length) {
  if (ippLiveLastResponse.length() >= 512) return;
  for (size_t i = 0; i < length && ippLiveLastResponse.length() < 512; ++i) {
    const char c = (char)data[i];
    if (c == '\r') continue;
    if (c == '\n') {
      if (ippLiveLastResponse.length()) break;
      continue;
    }
    if ((uint8_t)c >= 32 && (uint8_t)c <= 126) ippLiveLastResponse += c;
    else ippLiveLastResponse += '.';
  }
}

bool pumpLiveIppUsbIn(WiFiClient &client, bool &gotBytes, bool &finalResponseSeen,
                      uint32_t pollMs, String &error) {
  gotBytes = false;
  size_t received = 0;
  if (!usbHost.ippBulkReadPoll(ippLiveUsbInBuffer, sizeof(ippLiveUsbInBuffer), received,
                               pollMs, error)) return false;
  if (!received) return true;
  gotBytes = true;
  ++ippLiveInTransfers;
  ippLiveUsbToNet += received;
  noteLiveIppResponseBytes(ippLiveUsbInBuffer, received);

  size_t sent = 0;
  while (sent < received && client.connected()) {
    const size_t n = client.write(ippLiveUsbInBuffer + sent, received - sent);
    if (!n) {
      error = "TCP write failed while returning live IPP USB Bulk-IN";
      return false;
    }
    sent += n;
  }
  client.flush();

  // We only inspect a tiny response prefix for lifecycle control; bytes are
  // already forwarded unchanged to Android. 100 Continue is interim, while a
  // later HTTP status line is treated as the final response.
  static String responseProbe;
  if (responseProbe.length() < 2048) {
    for (size_t i = 0; i < received && responseProbe.length() < 2048; ++i) {
      responseProbe += (char)ippLiveUsbInBuffer[i];
    }
    int first = responseProbe.indexOf("HTTP/");
    while (first >= 0) {
      int eol = responseProbe.indexOf("\r\n", first);
      if (eol < 0) break;
      String line = responseProbe.substring(first, eol);
      const bool interim100 = line.indexOf(" 100 ") >= 0 || line.endsWith(" 100");
      if (!interim100) finalResponseSeen = true;
      first = responseProbe.indexOf("HTTP/", eol + 2);
    }
  }
  if (finalResponseSeen) responseProbe = "";
  return true;
}

void handleLiveIppUsb(WiFiClient &client, const String &networkHeader) {
  ippLiveActive = true;
  ippLiveNetToUsb = 0;
  ippLiveUsbToNet = 0;
  ippLiveOutTransfers = 0;
  ippLiveInTransfers = 0;
  ippLiveLastResponse = "";
  ippLiveLastError = "";

  const int8_t selected = usbHost.selectedIppInterfaceIndex();
  const UsbPrinterInterfaceInfo *iface = selected >= 0 ? usbHost.ippInterfaceAt((uint8_t)selected) : nullptr;
  if (!iface) {
    ippLiveLastError = "No explicitly selected IPP-over-USB interface";
    client.print("HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
    client.stop();
    ippLiveActive = false;
    return;
  }

  Serial.printf("[PROBE][IPP-LIVE] start candidate=%d IF=%u ALT=%u OUT=0x%02X IN=0x%02X\n",
                (int)selected, iface->interfaceNumber, iface->alternateSetting,
                iface->bulkOut.address, iface->bulkIn.address);

  const String usbHeader = normalizeLiveIppHeader(networkHeader);
  String error;
  if (!liveIppUsbWrite((const uint8_t *)usbHeader.c_str(), usbHeader.length(), error)) {
    ippLiveLastError = error;
    client.stop();
    ippLiveActive = false;
    return;
  }

  bool ok = true;
  bool finalResponseSeen = false;
  uint32_t lastAnyActivity = millis();
  uint32_t lastUsbIn = 0;
  uint32_t lastProgress = millis();

  while (ok && (client.connected() || client.available())) {
    bool didWork = false;

    // Forward at most one small TCP chunk per turn, then give Bulk-IN a chance.
    // This prevents a multi-megabyte Print-Job from starving printer replies.
    if (client.available() > 0) {
      const int want = min((int)sizeof(ippLiveNetBuffer), client.available());
      const int got = client.read(ippLiveNetBuffer, want);
      if (got > 0) {
        if (!liveIppUsbWrite(ippLiveNetBuffer, (size_t)got, error)) {
          ok = false;
          ippLiveLastError = error;
        } else {
          didWork = true;
          lastAnyActivity = millis();
        }
      }
    }

    if (ok) {
      bool gotUsb = false;
      if (!pumpLiveIppUsbIn(client, gotUsb, finalResponseSeen, 5, error)) {
        ok = false;
        ippLiveLastError = error;
      } else if (gotUsb) {
        didWork = true;
        lastAnyActivity = millis();
        lastUsbIn = millis();
      }
    }

    if (millis() - lastProgress >= 1000) {
      lastProgress = millis();
      Serial.printf("[PROBE][IPP-LIVE] net->usb=%llu usb->net=%llu OUT=%lu IN=%lu final=%d\n",
                    (unsigned long long)ippLiveNetToUsb,
                    (unsigned long long)ippLiveUsbToNet,
                    (unsigned long)ippLiveOutTransfers,
                    (unsigned long)ippLiveInTransfers,
                    finalResponseSeen ? 1 : 0);
    }

    // Once a non-100 HTTP response has arrived and Bulk-IN has gone quiet,
    // this request/response transaction is complete. Keep a short drain window
    // so the tail of the IPP response is not truncated.
    if (finalResponseSeen && lastUsbIn && millis() - lastUsbIn >= 300 && client.available() == 0) break;

    // Pure bridge mode intentionally waits for the real USB side. This catches
    // missing 100-Continue or a silent protocol-0x04 endpoint instead of faking
    // a local IPP response.
    if (!didWork && millis() - lastAnyActivity >= 30000) {
      ippLiveLastError = "live IPP bridge idle: no TCP body or USB response for 30 seconds";
      ok = false;
      break;
    }
    delay(1);
  }

  // Final short Bulk-IN drain while Android is still connected.
  if (client.connected()) {
    const uint32_t drainStart = millis();
    while (millis() - drainStart < 250) {
      bool gotUsb = false;
      String drainError;
      if (!pumpLiveIppUsbIn(client, gotUsb, finalResponseSeen, 5, drainError)) break;
      if (!gotUsb) delay(1);
    }
  }

  Serial.printf("[PROBE][IPP-LIVE] closed net->usb=%llu usb->net=%llu OUT=%lu IN=%lu ok=%d response='%s'%s%s\n",
                (unsigned long long)ippLiveNetToUsb,
                (unsigned long long)ippLiveUsbToNet,
                (unsigned long)ippLiveOutTransfers,
                (unsigned long)ippLiveInTransfers,
                ok ? 1 : 0, ippLiveLastResponse.c_str(),
                ippLiveLastError.length() ? " error=" : "",
                ippLiveLastError.c_str());
  client.stop();
  ippLiveActive = false;
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

  if (probeMode == MODE_IPP_LIVE) {
    handleLiveIppUsb(client, header);
    return;
  }

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

  lastRequest.seen = true;
  lastRequest.operation = op;
  lastRequest.requestId = requestId;
  lastRequest.transfer = chunked ? "chunked" : "content-length";
  lastRequest.format = format;
  lastRequest.jobName = jobName;

  if (op == 0x000B) {
    IppWriter w; buildPrinterAttributes(w, major, minor, requestId); sendHttpIpp(client, w);
    Serial.printf("[PROBE][IPP] TX successful-ok Get-Printer-Attributes request-id=%lu\n", (unsigned long)requestId);
  } else if (op == 0x0004) {
    Serial.printf("[PROBE][IPP] Validate-Job format=%s job=%s -> successful-ok\n", format.c_str(), jobName.c_str());
    noteStack("validate-job", true);
    sendSimpleSuccess(client, major, minor, requestId);
  } else if (op == 0x0002) {
    resetLastJob(requestId, chunked);
    lastJob.format = format;
    lastJob.source = jobName;
    lastJob.transport = modeName();
    Serial.printf("[PROBE][IPP] Print-Job format=%s job=%s; mode=%s\n",
                  format.c_str(), jobName.c_str(), modeName());
    noteStack("print-start", true);
    bool responseSent = false;
    const bool ok = consumeDocument(body, client, major, minor, requestId, format, jobName, responseSent);
    printLastJobSummary();
    if (!responseSent) sendJobResponse(client, major, minor, requestId, lastJob.jobId, ok);
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
  client.setNoDelay(true);

  rawBridgeActive = true;
  rawBridgeLastNetToUsb = 0;
  rawBridgeLastUsbToNet = 0;
  rawBridgeLastError = "";

  const UsbPrinterInterfaceInfo *raw = usbHost.selectedInterface();
  Serial.printf("[PROBE][RAW] LIVE duplex :9100 from %s:%u\n",
                client.remoteIP().toString().c_str(), client.remotePort());
  if (usbHost.state() != UsbHostManager::PRINTER_READY || !raw ||
      !raw->bulkOut.valid() || !raw->bulkIn.valid()) {
    rawBridgeLastError = "IF1-style classic printer Bulk-IN/Bulk-OUT interface is not ready";
    Serial.printf("[PROBE][RAW] bridge refused: %s\n", rawBridgeLastError.c_str());
    client.stop();
    rawBridgeActive = false;
    return;
  }

  Serial.printf("[PROBE][RAW] transparent bridge IF=%u ALT=%u proto=0x%02X OUT=0x%02X IN=0x%02X\n",
                raw->interfaceNumber, raw->alternateSetting, raw->protocol,
                raw->bulkOut.address, raw->bulkIn.address);

  uint32_t lastActivity = millis();
  uint32_t lastProgress = millis();
  bool bridgeOk = true;

  while ((client.connected() || client.available()) && bridgeOk) {
    // Wi-Fi -> USB Bulk OUT. Preserve bytes exactly; no parsing, PJL insertion,
    // PCL transformation, framing, or buffering of the full job.
    while (client.available() > 0 && bridgeOk) {
      const int want = min((int)sizeof(rawNetBuffer), client.available());
      const int got = client.read(rawNetBuffer, want);
      if (got <= 0) break;
      size_t offset = 0;
      while (offset < (size_t)got) {
        const size_t part = min((size_t)USB_CHUNK, (size_t)got - offset);
        size_t accepted = 0;
        String error;
        if (!usbHost.bulkWrite(rawNetBuffer + offset, part, accepted, 30000, error) || accepted != part) {
          rawBridgeLastError = error.length() ? error : "classic Bulk-OUT short write";
          bridgeOk = false;
          break;
        }
        offset += accepted;
        rawBridgeLastNetToUsb += accepted;
      }
      lastActivity = millis();
    }

    // USB Bulk IN -> same live TCP connection. Short 10 ms polls keep this
    // direction responsive without blocking the Wi-Fi -> OUT path.
    if (bridgeOk) {
      size_t received = 0;
      String error;
      if (!usbHost.bulkRead(rawUsbInBuffer, sizeof(rawUsbInBuffer), received, 10, error)) {
        rawBridgeLastError = error;
        bridgeOk = false;
      } else if (received) {
        size_t sent = 0;
        while (sent < received && client.connected()) {
          const size_t n = client.write(rawUsbInBuffer + sent, received - sent);
          if (!n) {
            rawBridgeLastError = "TCP write failed while forwarding printer Bulk-IN";
            bridgeOk = false;
            break;
          }
          sent += n;
        }
        rawBridgeLastUsbToNet += sent;
        lastActivity = millis();
      }
    }

    if (millis() - lastProgress >= 1000) {
      lastProgress = millis();
      Serial.printf("[PROBE][RAW] live net->usb=%llu usb->net=%llu\n",
                    (unsigned long long)rawBridgeLastNetToUsb,
                    (unsigned long long)rawBridgeLastUsbToNet);
    }

    // Keep a true session open while the client is connected. Five minutes of
    // total silence is only a safety escape for broken/stale sockets.
    if (millis() - lastActivity > 300000UL) {
      rawBridgeLastError = "RAW bridge idle timeout after 5 minutes";
      break;
    }
    delay(1);
  }

  // One final short IN drain lets immediate printer backchannel bytes reach a
  // still-connected client before close.
  if (client.connected()) {
    const uint32_t drainStart = millis();
    while (millis() - drainStart < 250) {
      size_t received = 0;
      String error;
      if (!usbHost.bulkRead(rawUsbInBuffer, sizeof(rawUsbInBuffer), received, 10, error)) break;
      if (received) {
        client.write(rawUsbInBuffer, received);
        rawBridgeLastUsbToNet += received;
      }
      delay(1);
    }
  }

  Serial.printf("[PROBE][RAW] LIVE closed net->usb=%llu usb->net=%llu ok=%d%s%s\n",
                (unsigned long long)rawBridgeLastNetToUsb,
                (unsigned long long)rawBridgeLastUsbToNet,
                bridgeOk ? 1 : 0,
                rawBridgeLastError.length() ? " error=" : "",
                rawBridgeLastError.c_str());
  client.stop();
  rawBridgeActive = false;
}

String hexString(const uint8_t *data, size_t n) {
  String s; s.reserve(n * 2 + 1); char b[3];
  for (size_t i = 0; i < n; ++i) { snprintf(b, sizeof(b), "%02X", data[i]); s += b; }
  return s;
}

String asciiPreview() {
  String out;
  for (size_t i = 0; i < min(lastJob.firstLen, (size_t)32); ++i) {
    const uint8_t b = lastJob.first[i]; out += (b >= 32 && b <= 126) ? (char)b : '.';
  }
  return out;
}

String jobSummaryHtml() {
  if (!lastJob.seen) return "<p>No Print-Job captured yet.</p>";
  String h = "<table>";
  h += "<tr><th>Job</th><td>" + String(lastJob.jobId) + "</td></tr>";
  h += "<tr><th>Format</th><td>" + htmlEscape(lastJob.format) + "</td></tr>";
  h += "<tr><th>Transfer</th><td>" + String(lastJob.chunked ? "chunked" : "content-length") + "</td></tr>";
  h += "<tr><th>Document bytes</th><td>" + String((unsigned long long)lastJob.documentBytes) + "</td></tr>";
  h += "<tr><th>Complete body</th><td>" + String(lastJob.bodyComplete ? "YES" : "NO") + "</td></tr>";
  h += "<tr><th>First ASCII</th><td><code>" + htmlEscape(asciiPreview()) + "</code></td></tr>";
  h += "<tr><th>FNV-1a</th><td>0x" + String(lastJob.fnv1a, HEX) + "</td></tr>";
  h += "<tr><th>Transport</th><td>" + htmlEscape(lastJob.transport) + "</td></tr>";
  h += "<tr><th>USB bytes accepted</th><td>" + String((unsigned long long)lastJob.usbAccepted) + "</td></tr>";
  h += "<tr><th>USB result</th><td>" + String(lastJob.usbAttempted ? (lastJob.usbSuccess ? "SUCCESS" : "FAILED") : "not attempted") + "</td></tr>";
  if (lastJob.usbError.length()) h += "<tr><th>Error</th><td>" + htmlEscape(lastJob.usbError) + "</td></tr>";
  h += "<tr><th>Physical result</th><td>" + String(lastJob.physicalResult > 0 ? "Printed" : (lastJob.physicalResult < 0 ? "No output" : "Unknown")) + "</td></tr>";
  h += "</table>";
  return h;
}

String lastRequestHtml() {
  if (!lastRequest.seen) return "<p>No Android IPP request observed yet.</p>";
  String h = "<table>";
  h += "<tr><th>Operation</th><td>" + String(ippOperationName(lastRequest.operation)) + " (0x" + String(lastRequest.operation, HEX) + ")</td></tr>";
  h += "<tr><th>Request ID</th><td>" + String(lastRequest.requestId) + "</td></tr>";
  h += "<tr><th>Transfer</th><td>" + htmlEscape(lastRequest.transfer) + "</td></tr>";
  h += "<tr><th>Document format</th><td>" + htmlEscape(lastRequest.format) + "</td></tr>";
  h += "<tr><th>Job name</th><td>" + htmlEscape(lastRequest.jobName) + "</td></tr>";
  h += "</table>";
  return h;
}

String recommendedAction() {
  if (!lastJob.seen) return "Leave Safe Capture selected and send the same photo from Android.";
  if (!lastJob.bodyComplete) return "Capture was incomplete. Keep Safe Capture selected and retry before any USB transport test.";
  if (probeMode == MODE_SAFE) return "Capture is complete. Select Classic USB RAW and send the same photo again.";
  if (lastJob.transport == "CLASSIC USB RAW") {
    if (!lastJob.usbSuccess) return "Classic USB RAW failed. Review the USB error before trying another transport.";
    if (lastJob.physicalResult == 0) return "USB accepted the full document. Check the printer, then mark Printed or No output below.";
    if (lastJob.physicalResult < 0) return "Classic USB RAW produced no page. Select IPP-over-USB Experimental and retry.";
    return "The page printed through Classic USB RAW. This transport works for the tested job.";
  }
  if (lastJob.transport == "IPP-OVER-USB EXPERIMENTAL") {
    if (lastJob.usbSuccess) return "IPP-over-USB returned a printer response. Check Android and the physical printer result.";
    return "IPP-over-USB failed. Try another protocol-0x04 interface candidate from the selector.";
  }
  return "Run the next test shown in Test Mode.";
}

void handleAdvertProfile() {
  if (!web.hasArg("profile")) { web.send(400, "text/plain", "Missing profile"); return; }
  const String profile = web.arg("profile");
  if (profile == "hp-broad") advertProfile = ADV_HP_INKJET_BROAD;
  else if (profile == "pcl3gui") advertProfile = ADV_PCL3GUI_ONLY;
  else if (profile == "pcl3gui-preferred") advertProfile = ADV_PCL3GUI_PREFERRED;
  else if (profile == "pclm") advertProfile = ADV_PCLM_ONLY;
  else if (profile == "urf") advertProfile = ADV_URF_ONLY;
  else if (profile == "pwg") advertProfile = ADV_PWG_ONLY;
  else if (profile == "jpeg") advertProfile = ADV_JPEG_ONLY;
  else if (profile == "auto") advertProfile = ADV_AUTOMATIC_ONLY;
  else if (profile == "pdf") advertProfile = ADV_PDF_EXPERIMENTAL;
  else { web.send(400, "text/plain", "Invalid profile"); return; }
  mdnsRefreshPending = true;
  Serial.printf("[PROBE][PDL] Advertisement changed to %s default=%s pdl=%s versions=%s\n",
                advertProfileName(), advertisedDefaultFormat(), advertisedPdlList().c_str(),
                advertisedVersionList().c_str());
  web.sendHeader("Location", "/"); web.send(303);
}

void handleMode() {
  if (!web.hasArg("mode")) { web.send(400, "text/plain", "Missing mode"); return; }
  const String mode = web.arg("mode");
  if (mode == "safe") probeMode = MODE_SAFE;
  else if (mode == "classic") probeMode = MODE_CLASSIC_RAW;
  else if (mode == "ippusb" || mode == "ipplive") {
    if (usbHost.selectedIppInterfaceIndex() < 0) {
      web.send(409, "text/plain", "Select and claim a protocol-0x04 IPP-over-USB interface first"); return;
    }
    probeMode = mode == "ipplive" ? MODE_IPP_LIVE : MODE_IPP_USB;
  } else { web.send(400, "text/plain", "Invalid mode"); return; }
  Serial.printf("[PROBE] Test mode changed to %s\n", modeName());
  web.sendHeader("Location", "/"); web.send(303);
}

void handleIppUsbSelect() {
  if (!web.hasArg("index")) { web.send(400, "text/plain", "Missing index"); return; }
  const int index = web.arg("index").toInt();
  String error;
  if (index < 0 || !usbHost.selectIppInterface((uint8_t)index, error)) {
    web.send(503, "text/plain", String("IPP-over-USB selection failed: ") + error); return;
  }
  web.sendHeader("Location", "/"); web.send(303);
}

void handlePhysical() {
  if (!lastJob.seen || !web.hasArg("result")) { web.send(400, "text/plain", "No job/result"); return; }
  lastJob.physicalResult = web.arg("result") == "printed" ? 1 : -1;
  web.sendHeader("Location", "/"); web.send(303);
}

void handleForwardToggle() {
  probeMode = web.hasArg("enable") && web.arg("enable") == "1" ? MODE_CLASSIC_RAW : MODE_SAFE;
  web.sendHeader("Location", "/"); web.send(303);
}

void handleWebRoot() {
  noteStack("dashboard", false);
  String html;
  html.reserve(15000);
  html += "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>Android Print Probe</title><style>body{font-family:system-ui;max-width:1000px;margin:24px auto;padding:0 14px;color:#263238}section{border:1px solid #ddd;border-radius:12px;padding:16px;margin:12px 0}button{padding:9px 13px;border-radius:8px;border:1px solid #999;font-weight:600;margin:4px}.warn{background:#fff4e5;border:1px solid #f0c36d;padding:10px;border-radius:8px}.good{background:#edf7ed;border:1px solid #8bc48b;padding:10px;border-radius:8px}table{border-collapse:collapse;width:100%}th,td{text-align:left;padding:7px;border-bottom:1px solid #eee}th{width:210px}code{word-break:break-all}label{display:block;margin:8px 0}</style></head><body>";
  html += "<h1>Android Print Probe — live duplex bridge</h1>";
  html += "<section><h2>Recommended next action</h2><div class='good'>" + htmlEscape(recommendedAction()) + "</div></section>";

  html += "<section><h2>1. Printer language advertisement</h2><p><b>Current: " + String(advertProfileName()) + "</b></p>";
  html += "<table><tr><th>Default MIME</th><td><code>" + String(advertisedDefaultFormat()) + "</code></td></tr>";
  html += "<tr><th>Supported MIME</th><td><code>" + htmlEscape(advertisedPdlList()) + "</code></td></tr>";
  html += "<tr><th>Versions / commands</th><td><code>" + htmlEscape(advertisedVersionList()) + "</code></td></tr></table>";
  html += "<form method='POST' action='/pdl'>";
  html += "<label><input type='radio' name='profile' value='hp-broad' " + String(advertProfile == ADV_HP_INKJET_BROAD ? "checked" : "") + "> HP inkjet broad — PCL3GUI + PCL3 + PJL + PCLm + URF + PWG + JPEG + Automatic</label>";
  html += "<label><input type='radio' name='profile' value='pcl3gui' " + String(advertProfile == ADV_PCL3GUI_ONLY ? "checked" : "") + "> PCL3GUI only — <code>application/vnd.hp-PCL</code>; strongest test to make HP Print Service render HP PCL</label>";
  html += "<label><input type='radio' name='profile' value='pcl3gui-preferred' " + String(advertProfile == ADV_PCL3GUI_PREFERRED ? "checked" : "") + "> PCL3GUI preferred — HP PCL first/default, but keep mobile fallbacks</label>";
  html += "<label><input type='radio' name='profile' value='pclm' " + String(advertProfile == ADV_PCLM_ONLY ? "checked" : "") + "> PCLm only — <code>application/PCLm</code></label>";
  html += "<label><input type='radio' name='profile' value='urf' " + String(advertProfile == ADV_URF_ONLY ? "checked" : "") + "> Apple Raster / URF only — <code>image/urf</code></label>";
  html += "<label><input type='radio' name='profile' value='pwg' " + String(advertProfile == ADV_PWG_ONLY ? "checked" : "") + "> PWG Raster only — <code>image/pwg-raster</code></label>";
  html += "<label><input type='radio' name='profile' value='jpeg' " + String(advertProfile == ADV_JPEG_ONLY ? "checked" : "") + "> JPEG only — <code>image/jpeg</code></label>";
  html += "<label><input type='radio' name='profile' value='auto' " + String(advertProfile == ADV_AUTOMATIC_ONLY ? "checked" : "") + "> Automatic only — <code>application/octet-stream</code></label>";
  html += "<label><input type='radio' name='profile' value='pdf' " + String(advertProfile == ADV_PDF_EXPERIMENTAL ? "checked" : "") + "> PDF experimental — <code>application/pdf</code> (not part of the Smart Tank-style profile)</label>";
  html += "<button>Apply advertisement</button></form><p class='warn'>Changing this updates IPP attributes immediately and refreshes mDNS. Android may cache capabilities; reopen the print dialog or remove/re-add the printer if the format does not change.</p></section>";

  html += "<section><h2>2. Test Mode</h2><p><b>Current: " + String(modeName()) + "</b></p><form method='POST' action='/mode'>";
  html += "<label><input type='radio' name='mode' value='safe' " + String(probeMode == MODE_SAFE ? "checked" : "") + "> Safe capture only — dechunk, parse and verify PCLm; no USB output</label>";
  html += "<label><input type='radio' name='mode' value='classic' " + String(probeMode == MODE_CLASSIC_RAW ? "checked" : "") + "> Classic USB RAW — extracted PCLm only to IF1-style Printer Class Bulk OUT</label>";
  html += "<label><input type='radio' name='mode' value='ippusb' " + String(probeMode == MODE_IPP_USB ? "checked" : "") + "> IPP-over-USB rebuilt — parse Android IPP, rebuild HTTP/IPP, then proxy through protocol 0x04</label>";
  html += "<label><input type='radio' name='mode' value='ipplive' " + String(probeMode == MODE_IPP_LIVE ? "checked" : "") + "> Live IPP USB duplex — pass HTTP/IPP body/chunk bytes directly to USB OUT and return USB IN live on the same TCP connection</label><button>Arm selected mode</button></form><p class='warn'>Live mode requires an explicitly selected protocol-0x04 candidate. It does not fake 100-Continue: the real USB endpoint must answer. Mode resets to Safe Capture after reboot.</p></section>";

  html += "<section><h2>3. USB printer status</h2><table><tr><th>State</th><td>" + htmlEscape(usbStateText()) + "</td></tr>";
  if (usbHost.device().attached) {
    html += "<tr><th>Printer</th><td>" + htmlEscape(usbHost.device().product) + "</td></tr>";
    html += "<tr><th>VID:PID</th><td>" + String(usbHost.device().vid, HEX) + ":" + String(usbHost.device().pid, HEX) + "</td></tr>";
  }
  const UsbPrinterInterfaceInfo *raw = usbHost.selectedInterface();
  if (raw) html += "<tr><th>RAW interface</th><td>IF" + String(raw->interfaceNumber) + " ALT" + String(raw->alternateSetting) + " protocol 0x" + String(raw->protocol, HEX) + " OUT 0x" + String(raw->bulkOut.address, HEX) + " IN 0x" + String(raw->bulkIn.address, HEX) + "</td></tr>";
  html += "</table></section>";

  html += "<section><h2>4. IPP-over-USB interface selector</h2>";
  const uint8_t ippCount = usbHost.ippInterfaceCount();
  if (!ippCount) html += "<p>No protocol-0x04 candidates detected.</p>";
  else {
    html += "<form method='POST' action='/ippusb'>";
    for (uint8_t i = 0; i < ippCount; ++i) {
      const UsbPrinterInterfaceInfo *it = usbHost.ippInterfaceAt(i); if (!it) continue;
      html += "<label><input type='radio' name='index' value='" + String(i) + "' " + String(usbHost.selectedIppInterfaceIndex() == (int8_t)i ? "checked" : "") + "> Candidate " + String(i) + ": IF" + String(it->interfaceNumber) + " ALT" + String(it->alternateSetting) + " OUT 0x" + String(it->bulkOut.address, HEX) + " IN 0x" + String(it->bulkIn.address, HEX) + "</label>";
    }
    html += "<button>Select interface</button></form>";
  }
  html += "</section>";

  html += "<section><h2>5. Last Android request</h2>" + lastRequestHtml() + "</section>";
  html += "<section><h2>6. Captured document / transport result</h2>" + jobSummaryHtml();
  if (lastJob.seen && lastJob.usbAttempted) {
    html += "<form method='POST' action='/physical'><button name='result' value='printed'>Printer printed page</button><button name='result' value='no'>No physical output</button></form>";
  }
  html += "</section>";

  html += "<section><h2>6. Live IPP 631 ↔ protocol-0x04 USB duplex</h2><table>";
  html += "<tr><th>Status</th><td>" + String(ippLiveActive ? "ACTIVE" : "idle") + "</td></tr>";
  html += "<tr><th>Network → USB OUT</th><td>" + String((unsigned long long)ippLiveNetToUsb) + " bytes</td></tr>";
  html += "<tr><th>USB IN → Network</th><td>" + String((unsigned long long)ippLiveUsbToNet) + " bytes</td></tr>";
  html += "<tr><th>OUT transfers</th><td>" + String((unsigned long)ippLiveOutTransfers) + "</td></tr>";
  html += "<tr><th>IN transfers with data</th><td>" + String((unsigned long)ippLiveInTransfers) + "</td></tr>";
  if (ippLiveLastResponse.length()) html += "<tr><th>Last USB HTTP response</th><td><code>" + htmlEscape(ippLiveLastResponse) + "</code></td></tr>";
  if (ippLiveLastError.length()) html += "<tr><th>Last error</th><td>" + htmlEscape(ippLiveLastError) + "</td></tr>";
  html += "</table><p>In Live IPP USB Duplex mode the ESP32 only normalizes Host/Connection headers. The IPP body and HTTP chunk bytes are forwarded unchanged, and protocol-0x04 Bulk-IN is polled between every small OUT transfer so 100-Continue, status and final responses can reach Android during the request.</p></section>";

  html += "<section><h2>7. Live RAW 9100 ↔ USB IF1 bridge</h2><table>";
  html += "<tr><th>Status</th><td>" + String(rawBridgeActive ? "ACTIVE" : "idle") + "</td></tr>";
  html += "<tr><th>Wi-Fi → USB OUT</th><td>" + String((unsigned long long)rawBridgeLastNetToUsb) + " bytes</td></tr>";
  html += "<tr><th>USB IN → Wi-Fi</th><td>" + String((unsigned long long)rawBridgeLastUsbToNet) + " bytes</td></tr>";
  if (rawBridgeLastError.length()) html += "<tr><th>Last error</th><td>" + htmlEscape(rawBridgeLastError) + "</td></tr>";
  html += "</table><p>TCP 9100 is byte-transparent and full-duplex: network bytes go directly to the selected classic Printer Class Bulk-OUT endpoint, and printer Bulk-IN bytes are returned live on the same TCP connection. No PDL conversion is performed.</p></section>";

  html += "<section><h2>8. Memory / stability</h2><table>";
  html += "<tr><th>Configured loop stack</th><td>" + String((unsigned)LOOP_STACK_CONFIGURED_BYTES) + " bytes</td></tr>";
  html += "<tr><th>Current stack free</th><td>" + String((unsigned)currentLoopStackFree()) + "</td></tr>";
  html += "<tr><th>Minimum stack free</th><td>" + String((unsigned)(minLoopStackFree == (size_t)-1 ? 0 : minLoopStackFree)) + "</td></tr>";
  html += "<tr><th>Free heap</th><td>" + String((unsigned)ESP.getFreeHeap()) + " bytes</td></tr>";
  html += "<tr><th>Free PSRAM</th><td>" + String((unsigned)ESP.getFreePsram()) + " bytes</td></tr></table></section>";

  html += "<section><h2>Dashboard steps</h2><p>1) Safe Capture → print from Android. 2) Confirm Complete body=YES and PCLm signature. 3) Select Classic USB RAW → print again. 4) Mark physical result. 5) For protocol-0x04 testing, explicitly select a candidate, then choose Rebuilt IPP or Live IPP USB Duplex. Live mode forwards the real printer HTTP response instead of synthesizing one.</p></section>";
  html += "</body></html>";
  web.send(200, "text/html; charset=utf-8", html);
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
  const String pdl = advertisedPdlList();
  const String cmd = advertisedVersionList();
  MDNS.addServiceTxt("ipp", "tcp", "pdl", pdl.c_str());
  MDNS.addServiceTxt("ipp", "tcp", "usb_MFG", "HP");
  MDNS.addServiceTxt("ipp", "tcp", "usb_MDL", MODEL);
  MDNS.addServiceTxt("ipp", "tcp", "usb_CMD", cmd.c_str());
  MDNS.addService("pdl-datastream", "tcp", RAW_PORT);
  MDNS.addServiceTxt("pdl-datastream", "tcp", "txtvers", "1");
  MDNS.addServiceTxt("pdl-datastream", "tcp", "ty", MODEL);
  MDNS.addService("http", "tcp", 80);
}
} // namespace

void setup() {
  Serial.begin(115200); delay(500);
  Serial.println();
  Serial.println("=== ESP32-S3 Android Print Probe — one-flash diagnostic dashboard ===");
  Serial.printf("[PROBE] Loop stack configured: %u bytes\n", (unsigned)LOOP_STACK_CONFIGURED_BYTES);
  Serial.println("[PROBE] Safe Capture is the default after every reboot; TCP 9100 is always a transparent IF1 duplex bridge");
  noteStack("boot", true);

  if (!connectSavedWiFi()) startProbeAp();
  advertiseProbe();

  if (!usbHost.begin()) Serial.printf("[USB] Host start failed: %s\n", usbHost.lastError().c_str());
  else Serial.println("[USB] Host started; connect the HP printer whenever ready");

  ippServer.begin(); rawServer.begin(); ippServer.setNoDelay(true); rawServer.setNoDelay(true);
  web.on("/", HTTP_GET, handleWebRoot);
  web.on("/pdl", HTTP_POST, handleAdvertProfile);
  web.on("/mode", HTTP_POST, handleMode);
  web.on("/ippusb", HTTP_POST, handleIppUsbSelect);
  web.on("/physical", HTTP_POST, handlePhysical);
  web.on("/forward", HTTP_POST, handleForwardToggle);
  web.on("/webApps/images/printer.png", HTTP_GET, handlePrinterIcon);
  web.begin();

  const String ip = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  Serial.printf("[PROBE] Dashboard: http://%s/ (or http://printer.local/)\n", ip.c_str());
  Serial.println("[PROBE] First run: leave SAFE CAPTURE selected and print once from HP Print Service.");
}

void loop() {
  web.handleClient();
  if (mdnsRefreshPending) {
    mdnsRefreshPending = false;
    advertiseProbe();
    Serial.printf("[PROBE][PDL] mDNS refreshed for %s\n", advertProfileName());
  }
  usbHost.poll();
  serviceIpp();
  serviceRaw();
  static unsigned long lastMem = 0;
  if (millis() - lastMem >= 1000) {
    lastMem = millis();
    noteStack("loop", false);
  }
  delay(1);
}
