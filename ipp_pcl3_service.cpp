#include "ipp_pcl3_service.h"
#include "logger.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_mac.h"
#include "esp_task_wdt.h"
#include "esp_heap_caps.h"

String printerUuid();

namespace {
constexpr uint32_t CLIENT_TIMEOUT_MS = IPP_CLIENT_TIMEOUT_MS;  // kept below the 60 s watchdog
constexpr size_t HTTP_HEADER_LIMIT = 4096;
constexpr size_t BODY_BUFFER = IPP_BODY_BUFFER_SIZE;
// MIME type tokens are case-insensitive.  Use the registered lowercase form
// in responses, while accepting Android clients that vary the casing.
constexpr const char *PCL3_MIME = PRINTER_DOCUMENT_FORMAT;

WiFiServer ippServer(IPP_PORT);
static uint8_t *bodyBuffer = nullptr;
// One request is handled at a time, so a single shared response buffer is
// enough and keeps 8 KB off the loop task's stack.
static uint8_t responseBuffer[IPP_RESPONSE_MAX];

bool readClientByte(WiFiClient &client, uint8_t &out, uint32_t timeoutMs) {
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

size_t readClientBlock(WiFiClient &client, uint8_t *dst, size_t capacity, uint32_t timeoutMs) {
  if (!dst || !capacity) return 0;
  const uint32_t started = millis();
  while (client.connected() || client.available()) {
    const int available = client.available();
    if (available > 0) {
      const size_t want = min(capacity, (size_t)available);
      const int got = client.read(dst, want);
      if (got > 0) return (size_t)got;
    }
    if (millis() - started >= timeoutMs) return 0;
    delay(1);
  }
  return 0;
}

bool readHttpHeader(WiFiClient &client, String &header) {
  header = "";
  header.reserve(1024);
  uint8_t b = 0;
  while (header.length() < HTTP_HEADER_LIMIT) {
    if (!readClientByte(client, b, 10000)) return false;
    header += (char)b;
    if (header.endsWith("\r\n\r\n")) return true;
  }
  return false;
}

String headerValue(const String &header, const char *name) {
  String lower = header; lower.toLowerCase();
  String key = String(name); key.toLowerCase(); key += ":";
  const int at = lower.indexOf(key);
  if (at < 0) return "";
  int p = at + key.length();
  while (p < (int)header.length() && (header[p] == ' ' || header[p] == '\t')) ++p;
  int end = header.indexOf("\r\n", p);
  if (end < 0) end = header.length();
  String value = header.substring(p, end); value.trim(); return value;
}

uint32_t upTimeSeconds() { return (uint32_t)(millis() / 1000ULL); }

time_t epochNow() {
  time_t n = time(nullptr);
  // No RTC/NTP yet: anchor to a fixed date plus uptime so dateTime values are
  // at least monotonic and structurally valid.
  if (n < 1600000000) n = (time_t)1704067200 + (time_t)upTimeSeconds();
  return n;
}

String hostForUri() {
  IPAddress ip = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP() : WiFi.softAPIP();
  if ((uint32_t)ip != 0) return ip.toString();
  return "printer.local";
}

String ippBaseUri() {
  return String("ipp://") + hostForUri() + ":" + String(IPP_PORT) + "/ipp/print";
}

String httpBaseUri() { return String("http://") + hostForUri(); }

struct BodyReader {
  WiFiClient &client;
  bool chunked = false;
  int64_t remaining = -1;
  size_t chunkRemaining = 0;
  bool done = false;
  bool framingError = false;
  bool peerClosed = false;

  explicit BodyReader(WiFiClient &c) : client(c) {}

  bool rawByte(uint8_t &b) {
    if (readClientByte(client, b, CLIENT_TIMEOUT_MS)) return true;
    if (!client.connected() && client.available() == 0) peerClosed = true;
    return false;
  }

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
      if (!rawLine(line)) {
        // A peer that closes exactly at a chunk boundary without sending the
        // terminating 0-chunk has still delivered every byte it produced; leave
        // framingError clear so the caller may accept the document.  A partial
        // size line, or a timeout while still connected, is a real error.
        if (!(peerClosed && line.length() == 0)) framingError = true;
        return false;
      }
    } while (line.length() == 0);

    const int semi = line.indexOf(';');
    if (semi >= 0) line = line.substring(0, semi);
    line.trim();
    char *endp = nullptr;
    const unsigned long n = strtoul(line.c_str(), &endp, 16);
    if (!endp || *endp != 0) { framingError = true; return false; }

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
    if (done || framingError) return false;
    if (!chunked) {
      if (remaining == 0) { done = true; return false; }
      if (!rawByte(b)) return false;
      if (remaining > 0) --remaining;
      if (remaining == 0) done = true;
      return true;
    }

    if (chunkRemaining == 0 && !nextChunk()) return false;
    if (!rawByte(b)) return false;
    --chunkRemaining;
    if (chunkRemaining == 0) {
      uint8_t cr = 0, lf = 0;
      if (!rawByte(cr) || !rawByte(lf) || cr != '\r' || lf != '\n') {
        framingError = true;
        return false;
      }
    }
    return true;
  }

  bool readExact(uint8_t *dst, size_t n) {
    for (size_t i = 0; i < n; ++i) if (!readByte(dst[i])) return false;
    return true;
  }

  // True when the peer stopped after a complete chunk (CRLF consumed) but
  // without sending the terminating zero-length chunk. Some Android clients
  // close the socket instead of terminating the chunked body.
  bool atChunkBoundary() const {
    return chunked && !framingError && !done && chunkRemaining == 0;
  }

  // Fast document path. IPP headers/attributes are still parsed byte-wise, but
  // after the end-of-attributes tag the document is dechunked into large blocks.
  size_t readBlock(uint8_t *dst, size_t capacity) {
    if (!dst || !capacity || done || framingError) return 0;

    if (!chunked) {
      if (remaining == 0) { done = true; return 0; }
      size_t want = capacity;
      if (remaining > 0) want = min(want, (size_t)remaining);
      const size_t got = readClientBlock(client, dst, want, CLIENT_TIMEOUT_MS);
      if (!got) {
        if (!client.connected() && client.available() == 0) {
          peerClosed = true;
          if (remaining < 0) done = true;
        }
        return 0;
      }
      if (remaining > 0) {
        remaining -= (int64_t)got;
        if (remaining == 0) done = true;
      }
      return got;
    }

    size_t total = 0;
    while (total < capacity && !done && !framingError) {
      if (chunkRemaining == 0) {
        if (!nextChunk()) break;
      }
      const size_t room = capacity - total;
      if (room == 0) break;  // defensive: never issue a zero-length read
      const size_t want = (chunkRemaining < room) ? chunkRemaining : room;
      if (want == 0) break;

      const size_t got = readClientBlock(client, dst + total, want, CLIENT_TIMEOUT_MS);
      if (!got) {
        if (!client.connected() && client.available() == 0) peerClosed = true;
        break;
      }

      total += got;
      chunkRemaining -= got;

      if (chunkRemaining == 0) {
        uint8_t cr = 0, lf = 0;
        if (!rawByte(cr) || !rawByte(lf) || cr != '\r' || lf != '\n') {
          framingError = true;
          break;
        }
      }
    }
    return total;
  }
};

bool supportedDocumentFormat(const String &format) {
  // IPP mimeMediaType values are case-insensitive.  Android commonly sends
  // application/vnd.hp-pcl while older clients may use application/vnd.hp-PCL;
  // octet-stream means "raw" and is piped straight to the printer as well.
  if (format.equalsIgnoreCase(PCL3_MIME)) return true;
#if PRINTER_PCLM_PASSTHROUGH
  // PCLm is a host-rasterised PDF subset; we forward it unchanged and let the
  // downstream printer decide whether it can interpret it.
  if (format.equalsIgnoreCase(PRINTER_PCLM_MIME)) return true;
#endif
  return format.equalsIgnoreCase("application/octet-stream");
}

struct IppWriter {
  uint8_t *data = responseBuffer;
  size_t cap = IPP_RESPONSE_MAX;
  size_t len = 0;
  bool ok = true;
  void b(uint8_t v){ if(len < cap) data[len++] = v; else ok = false; }
  void u16(uint16_t v){ b(v >> 8); b(v); }
  void u32(uint32_t v){ b(v >> 24); b(v >> 16); b(v >> 8); b(v); }
  void raw(const uint8_t *p,size_t n){ if(!ok || len + n > cap){ok=false;return;} memcpy(data+len,p,n); len+=n; }
  void attr(uint8_t tag,const char *name,const uint8_t *value,uint16_t valueLen){
    const uint16_t nameLen = name ? (uint16_t)strlen(name) : 0;
    b(tag); u16(nameLen); if(nameLen) raw((const uint8_t*)name,nameLen); u16(valueLen); if(valueLen) raw(value,valueLen);
  }
  void str(uint8_t tag,const char *name,const char *value){ attr(tag,name,(const uint8_t*)value,(uint16_t)strlen(value)); }
  void integer(uint8_t tag,const char *name,int32_t value){ uint8_t v[4]={(uint8_t)((uint32_t)value>>24),(uint8_t)((uint32_t)value>>16),(uint8_t)((uint32_t)value>>8),(uint8_t)value}; attr(tag,name,v,4); }
  void boolean(const char *name,bool value){ const uint8_t v=value?1:0; attr(0x22,name,&v,1); }
  // rangeOfInteger (0x33): low i32 then high i32
  void range(const char *name,int32_t lo,int32_t hi){ uint8_t v[8]={(uint8_t)((uint32_t)lo>>24),(uint8_t)((uint32_t)lo>>16),(uint8_t)((uint32_t)lo>>8),(uint8_t)lo,(uint8_t)((uint32_t)hi>>24),(uint8_t)((uint32_t)hi>>16),(uint8_t)((uint32_t)hi>>8),(uint8_t)hi}; attr(0x33,name,v,8); }
  // resolution (0x32): cross-feed i32, feed i32, units octet (3 = dpi).
  // Verified against a real HP Get-Printer-Attributes dump:
  //   32 00 1C "printer-resolution-supported" 00 09 00 00 01 2C 00 00 01 2C 03
  // NOTE: 0x35 is textWithLanguage, not resolution.
  void resolution(const char *name,int32_t dotsPerInch){ uint8_t v[9]={(uint8_t)((uint32_t)dotsPerInch>>24),(uint8_t)(((uint32_t)dotsPerInch>>16)&0xFF),(uint8_t)(((uint32_t)dotsPerInch>>8)&0xFF),(uint8_t)((uint32_t)dotsPerInch&0xFF),(uint8_t)((uint32_t)dotsPerInch>>24),(uint8_t)(((uint32_t)dotsPerInch>>16)&0xFF),(uint8_t)(((uint32_t)dotsPerInch>>8)&0xFF),(uint8_t)((uint32_t)dotsPerInch&0xFF),3}; attr(0x32,name,v,9); }
  void dateTime(const char *name,time_t when){
    struct tm tmv; gmtime_r(&when,&tmv);
    uint8_t v[11];
    const int year = tmv.tm_year + 1900;
    v[0]=(uint8_t)(year>>8); v[1]=(uint8_t)year;
    v[2]=(uint8_t)(tmv.tm_mon+1); v[3]=(uint8_t)tmv.tm_mday;
    v[4]=(uint8_t)tmv.tm_hour; v[5]=(uint8_t)tmv.tm_min; v[6]=(uint8_t)tmv.tm_sec;
    v[7]=0; v[8]='+'; v[9]=0; v[10]=0;
    attr(0x31,name,v,11);
  }
  // Collection support (RFC 8010 3.1.6). A member is: memberAttrName, value.
  void begCollection(const char *name){ attr(0x34,name,nullptr,0); }
  void memberName(const char *member){ attr(0x4A,nullptr,(const uint8_t*)member,(uint16_t)strlen(member)); }
  void endCollection(){ attr(0x37,nullptr,nullptr,0); }
};

void beginResponse(IppWriter &w,uint8_t major,uint8_t minor,uint16_t status,uint32_t requestId){
  w.b(major?major:2); w.b(major?minor:0); w.u16(status); w.u32(requestId); w.b(0x01);
  w.str(0x47,"attributes-charset","utf-8"); w.str(0x48,"attributes-natural-language","en");
}

void sendHttpIpp(WiFiClient &client,const IppWriter &w,bool keepAlive){
  client.print("HTTP/1.1 200 OK\r\nContent-Type: application/ipp\r\n");
  client.print(keepAlive ? "Connection: keep-alive\r\n" : "Connection: close\r\n");
  client.printf("Content-Length: %u\r\n\r\n",(unsigned)w.len);
  client.write(w.data,w.len);
  client.flush();
}

void sendSimple(WiFiClient &client,uint8_t major,uint8_t minor,uint16_t status,uint32_t requestId,bool keepAlive){
  IppWriter w; beginResponse(w,major,minor,status,requestId); w.b(0x03); sendHttpIpp(client,w,keepAlive);
}

// --- requested-attributes filtering -------------------------------------
struct RequestAttrs {
  String documentFormat;
  bool hasJobId = false;
  int32_t jobId = 0;
  bool hasJobUri = false;
  String jobUri;
  bool hasLastDocument = false;
  bool lastDocument = true;
  String jobName;
  // The HP plugin asks for ~68 attributes in one Get-Printer-Attributes
  // request.  The old 24-entry cap silently dropped everything after the
  // first block, which hid sides, margins, print-scaling and colour.
  static constexpr uint8_t MAX_REQUESTED = 96;
  String requested[MAX_REQUESTED];
  uint8_t requestedCount = 0;
};

struct AttrFilter {
  const RequestAttrs *req = nullptr;
  bool all = true;
  bool description = false;
  bool jobTemplate = false;

  explicit AttrFilter(const RequestAttrs &r) : req(&r) {
    if (r.requestedCount == 0) return;
    all = false;
    for (uint8_t i = 0; i < r.requestedCount; ++i) {
      const String &v = r.requested[i];
      if (v.equalsIgnoreCase("all")) { all = true; continue; }
      if (v.equalsIgnoreCase("printer-description")) { description = true; continue; }
      if (v.equalsIgnoreCase("job-template")) { jobTemplate = true; continue; }
      // document-description only carries document-format-* here, which are
      // also printer-description attributes; treat it as the same group.
      if (v.equalsIgnoreCase("document-description")) { description = true; continue; }
    }
  }

  bool want(const char *name, bool tmpl = false) const {
    if (all) return true;
    if (req) {
      for (uint8_t i = 0; i < req->requestedCount; ++i)
        if (req->requested[i].equalsIgnoreCase(name)) return true;
    }
    return tmpl ? jobTemplate : description;
  }
};

// --- printer state snapshot ---------------------------------------------
struct PrinterSnapshot {
  uint8_t ippState = 5;
  String reasons = "offline";
  bool accepting = false;
  uint32_t queued = 0;
  uint32_t upTime = 0;
  uint32_t stateChangeUp = 0;
  uint32_t configChangeUp = 0;
  String alertCode = "otherError";
  String alertSeverity = "critical";
  String alertGroup = "generalPrinter";
  String alertDescription = "otherError";
};

void addJobAttributes(IppWriter &w,uint32_t jobId,uint8_t jobState,const String &reason,
                      const String &jobName,uint32_t upTime,uint32_t processingUp,uint32_t completedUp){
  w.b(0x02);
  w.integer(0x21,"job-id",(int32_t)jobId);
  const String base = ippBaseUri();
  const String jobUri = base + "/job/" + String(jobId);
  w.str(0x45,"job-uri",jobUri.c_str());
  w.str(0x45,"job-printer-uri",base.c_str());
  w.integer(0x23,"job-state",jobState);
  w.str(0x44,"job-state-reasons",reason.length()?reason.c_str():"none");
  if(jobName.length()) w.str(0x42,"job-name",jobName.c_str());
  w.integer(0x21,"job-printer-up-time",(int32_t)upTime);
  if(processingUp) w.integer(0x21,"time-at-processing",(int32_t)processingUp);
  if(completedUp) w.integer(0x21,"time-at-completed",(int32_t)completedUp);
}

void sendJobResponse(WiFiClient &client,uint8_t major,uint8_t minor,uint16_t status,uint32_t requestId,
                     uint32_t jobId,uint8_t jobState,const String &reason,const String &jobName,
                     uint32_t upTime,uint32_t processingUp,uint32_t completedUp,bool keepAlive){
  IppWriter w; beginResponse(w,major,minor,status,requestId);
  if(status==0x0000 && jobId) addJobAttributes(w,jobId,jobState,reason,jobName,upTime,processingUp,completedUp);
  w.b(0x03); sendHttpIpp(client,w,keepAlive);
}

// --- media collections ---------------------------------------------------
struct MediaDef {
  const char *name;
  int32_t x;
  int32_t y;
  bool borderless;
  bool ready;
  const char *type;
};

// Sizes from the Smart Tank 520/540 data sheet.  Borderless is supported up
// to A4/Letter and on 10x15 cm photo paper.
static const MediaDef MEDIA_DEFS[] = {
  {"iso_a4_210x297mm",         21000, 29700, true,  true,  "stationery"},
  {"na_letter_8.5x11in",       21590, 27940, true,  true,  "stationery"},
  {"om_small-photo_100x150mm", 10000, 15000, true,  false, "photographic"},
  {"iso_a6_105x148mm",         10500, 14800, false, false, "stationery"},
  {"jis_b5_182x257mm",         18200, 25700, false, false, "stationery"},
  {"na_legal_8.5x14in",        21590, 35560, false, false, "stationery"},
  {"iso_dl_110x220mm",         11000, 22000, false, false, "stationery"},
};
static const size_t MEDIA_COUNT = sizeof(MEDIA_DEFS) / sizeof(MEDIA_DEFS[0]);

void writeMediaSize(IppWriter &w,const char *attrName,int32_t x,int32_t y){
  w.begCollection(attrName);
  w.memberName("x-dimension"); w.integer(0x21,nullptr,x);
  w.memberName("y-dimension"); w.integer(0x21,nullptr,y);
  w.endCollection();
}

// margin is in hundredths of a millimetre; a negative value omits the margin
// members entirely (used by media-col-ready, like HP does).
void writeMediaCol(IppWriter &w,const char *attrName,const MediaDef &media,int32_t margin){
  w.begCollection(attrName);
  w.memberName("media-size");
  w.begCollection(nullptr);
  w.memberName("x-dimension"); w.integer(0x21,nullptr,media.x);
  w.memberName("y-dimension"); w.integer(0x21,nullptr,media.y);
  w.endCollection();
  w.memberName("media-size-name"); w.str(0x44,nullptr,media.name);
  w.memberName("media-type"); w.str(0x44,nullptr,media.type);
  w.memberName("media-source"); w.str(0x44,nullptr,"main");
  if(margin>=0){
    w.memberName("media-left-margin");   w.integer(0x21,nullptr,margin);
    w.memberName("media-right-margin");  w.integer(0x21,nullptr,margin);
    w.memberName("media-top-margin");    w.integer(0x21,nullptr,margin);
    w.memberName("media-bottom-margin"); w.integer(0x21,nullptr,margin);
  }
  w.endCollection();
}

// --- Get-Printer-Attributes ---------------------------------------------
void sendPrinterAttributes(WiFiClient &client,uint8_t major,uint8_t minor,uint32_t requestId,
                           const RequestAttrs &req,const PrinterSnapshot &snap,bool keepAlive){
  const AttrFilter f(req);
  IppWriter w; beginResponse(w,major,minor,0x0000,requestId); w.b(0x04);

  // --- minimum required set: always returned, regardless of the filter ---
  const String base = ippBaseUri();
  w.str(0x45,"printer-uri-supported",base.c_str());
  w.str(0x44,"uri-security-supported","none");
  w.str(0x44,"uri-authentication-supported","requesting-user-name");
  w.str(0x42,"printer-name",PRINTER_INSTANCE_NAME);
  w.integer(0x23,"printer-state",snap.ippState);
  w.str(0x44,"printer-state-reasons",snap.reasons.c_str());
  w.boolean("printer-is-accepting-jobs",snap.accepting);
  w.str(0x44,"ipp-versions-supported","1.0"); w.str(0x44,nullptr,"1.1"); w.str(0x44,nullptr,"2.0");
  w.integer(0x23,"operations-supported",0x0002); w.integer(0x23,nullptr,0x0004);
  w.integer(0x23,nullptr,0x0005); w.integer(0x23,nullptr,0x0006);
  w.integer(0x23,nullptr,0x0008); w.integer(0x23,nullptr,0x0009);
  w.integer(0x23,nullptr,0x000A); w.integer(0x23,nullptr,0x000B);
  w.integer(0x23,nullptr,0x003B);
  w.str(0x47,"charset-configured","utf-8");
  w.str(0x47,"charset-supported","utf-8"); w.str(0x47,nullptr,"us-ascii");
  w.str(0x48,"natural-language-configured","en");
  w.str(0x48,"generated-natural-language-supported","en");
  w.str(0x49,"document-format-supported",PCL3_MIME);
#if PRINTER_PCLM_PASSTHROUGH
  w.str(0x49,nullptr,PRINTER_PCLM_MIME);
#endif
  w.str(0x49,nullptr,"application/octet-stream");
  w.str(0x49,"document-format-default",PCL3_MIME);

  // --- descriptive ---
  if (f.want("printer-info")) w.str(0x41,"printer-info",PRINTER_MAKE_MODEL);
  if (f.want("printer-make-and-model")) w.str(0x41,"printer-make-and-model",PRINTER_MAKE_MODEL);
  if (f.want("printer-location")) w.str(0x41,"printer-location","USB print bridge");
  if (f.want("printer-dns-sd-name")) w.str(0x42,"printer-dns-sd-name",PRINTER_INSTANCE_NAME);
  if (f.want("printer-more-info")) {
    const String more = httpBaseUri() + "/";
    w.str(0x45,"printer-more-info",more.c_str());
  }
  if (f.want("printer-uuid")) w.str(0x45,"printer-uuid",printerUuid().c_str());
  if (f.want("printer-device-id")) w.str(0x41,"printer-device-id",PRINTER_DEVICE_ID);
  if (f.want("printer-kind")) {
    w.str(0x44,"printer-kind","document");
    w.str(0x44,nullptr,"envelope");
    w.str(0x44,nullptr,"photo");
  }
  if (f.want("printer-icons")) {
    const String icons = httpBaseUri();
    const String small = icons + "/icon-48.png";
    const String medium = icons + "/icon-128.png";
    const String large = icons + "/icon-512.png";
    w.str(0x45,"printer-icons",small.c_str());
    w.str(0x45,nullptr,medium.c_str());
    w.str(0x45,nullptr,large.c_str());
  }
  if (f.want("printer-strings-uri")) {
    // We do not ship a localisation catalogue; advertise nothing rather than a
    // URI that would 404.
  }

  // --- status / alerts ---------------------------------------------------
  if (f.want("queued-job-count")) w.integer(0x21,"queued-job-count",(int32_t)snap.queued);
  if (f.want("printer-up-time")) w.integer(0x21,"printer-up-time",(int32_t)snap.upTime);
  if (f.want("printer-current-time")) w.dateTime("printer-current-time",epochNow());
  if (f.want("printer-config-change-time")) w.integer(0x21,"printer-config-change-time",(int32_t)snap.configChangeUp);
  if (f.want("printer-config-change-date-time"))
    w.dateTime("printer-config-change-date-time",epochNow() - (time_t)(snap.upTime - snap.configChangeUp));
  if (f.want("printer-state-change-time")) w.integer(0x21,"printer-state-change-time",(int32_t)snap.stateChangeUp);
  if (f.want("printer-state-change-date-time"))
    w.dateTime("printer-state-change-date-time",epochNow() - (time_t)(snap.upTime - snap.stateChangeUp));

  if (f.want("printer-alert")) {
    const String alert = String("code=") + snap.alertCode + ";severity=" + snap.alertSeverity +
                         ";group=" + snap.alertGroup;
    w.str(0x30,"printer-alert",alert.c_str());
  }
  if (f.want("printer-alert-description")) w.str(0x41,"printer-alert-description",snap.alertDescription.c_str());
  if (f.want("printer-input-tray")) {
    const String tray = "type=inputTray;maxcapacity=150;level=-2;status=0;name=Main tray";
    w.str(0x30,"printer-input-tray",tray.c_str());
  }
  if (f.want("printer-output-tray")) {
    const String tray = "type=outputTray;maxcapacity=-2;level=-2;status=0;name=Output tray";
    w.str(0x30,"printer-output-tray",tray.c_str());
  }

  // --- job / document capabilities --------------------------------------
  if (f.want("multiple-document-jobs-supported")) w.boolean("multiple-document-jobs-supported",false);
  if (f.want("multiple-operation-time-out")) w.integer(0x21,"multiple-operation-time-out",120);
  if (f.want("compression-supported")) w.str(0x44,"compression-supported","none");
  if (f.want("pdl-override-supported")) w.str(0x44,"pdl-override-supported","attempted");
  if (f.want("which-jobs-supported")) {
    w.str(0x44,"which-jobs-supported","completed");
    w.str(0x44,nullptr,"not-completed");
    w.str(0x44,nullptr,"all");
  }
  if (f.want("job-ids-supported")) w.boolean("job-ids-supported",true);
  if (f.want("printer-settable-attributes-supported")) w.str(0x44,"printer-settable-attributes-supported","none");
  if (f.want("requesting-user-uri-supported")) w.boolean("requesting-user-uri-supported",false);
  if (f.want("printer-get-attributes-supported")) w.str(0x44,"printer-get-attributes-supported","document-format");

  if (f.want("job-creation-attributes-supported")) {
    w.str(0x44,"job-creation-attributes-supported","copies");
    w.str(0x44,nullptr,"job-name");
    w.str(0x44,nullptr,"document-format");
    w.str(0x44,nullptr,"document-name");
    w.str(0x44,nullptr,"ipp-attribute-fidelity");
    w.str(0x44,nullptr,"sides");
    w.str(0x44,nullptr,"media");
    w.str(0x44,nullptr,"media-col");
    w.str(0x44,nullptr,"print-quality");
    w.str(0x44,nullptr,"printer-resolution");
    w.str(0x44,nullptr,"orientation-requested");
    w.str(0x44,nullptr,"print-color-mode");
    w.str(0x44,nullptr,"print-scaling");
  }

  // --- job template ------------------------------------------------------
  // HP reports one text value per personality (7 on the 6830), not a single
  // comma-joined string.  List only the formats we accept.
  if (f.want("document-format-version-supported",true)) {
    w.str(0x41,"document-format-version-supported","PCL3GUI");
    w.str(0x41,nullptr,"PCL3");
    w.str(0x41,nullptr,"PJL");
    w.str(0x41,nullptr,"Automatic");
#if PRINTER_PCLM_PASSTHROUGH
    w.str(0x41,nullptr,"PCLM");
#endif
  }

#if PRINTER_PCLM_PASSTHROUGH
  // PCLm generation parameters, mirrored from a real HP device.  These tell
  // the host rasteriser which subset the downstream printer can consume:
  // RLE-compressed strips of 32 rows, 300/600 dpi source, rotated back side.
  if (f.want("pclm-compression-method-preferred",true)) {
    w.str(0x44,"pclm-compression-method-preferred","rle");
  }
  if (f.want("pclm-source-resolution-supported",true)) {
    w.resolution("pclm-source-resolution-supported",300);
    w.resolution(nullptr,600);
  }
  if (f.want("pclm-source-resolution-default",true)) {
    w.resolution("pclm-source-resolution-default",600);
  }
  if (f.want("pclm-strip-height-supported",true)) {
    w.integer(0x21,"pclm-strip-height-supported",32);
  }
  if (f.want("pclm-strip-height-preferred",true)) {
    w.integer(0x21,"pclm-strip-height-preferred",32);
  }
  if (f.want("pclm-raster-back-side",true)) {
    w.str(0x44,"pclm-raster-back-side","rotated");
  }
#endif

  if (f.want("printer-resolution-supported",true)) {
    w.resolution("printer-resolution-supported",300);
  }
  if (f.want("printer-resolution-default",true)) {
    w.resolution("printer-resolution-default",300);
  }

  if (f.want("print-quality-supported",true)) {
    w.integer(0x23,"print-quality-supported",4);
    w.integer(0x23,nullptr,5);
  }
  if (f.want("print-quality-default",true)) w.integer(0x23,"print-quality-default",4);

  if (f.want("media-supported",true)) {
    for(size_t i=0;i<MEDIA_COUNT;++i)
      w.str(0x44,i?"media-supported":nullptr,MEDIA_DEFS[i].name);
  }
  if (f.want("media-default",true)) w.str(0x44,"media-default","iso_a4_210x297mm");
  if (f.want("media-ready",true)) {
    bool first=true;
    for(size_t i=0;i<MEDIA_COUNT;++i){
      if(!MEDIA_DEFS[i].ready) continue;
      w.str(0x44,first?"media-ready":nullptr,MEDIA_DEFS[i].name);
      first=false;
    }
  }
  if (f.want("media-size-supported",true)) {
    for(size_t i=0;i<MEDIA_COUNT;++i)
      writeMediaSize(w,i?"media-size-supported":nullptr,MEDIA_DEFS[i].x,MEDIA_DEFS[i].y);
  }
  if (f.want("media-col-default",true)) {
    writeMediaCol(w,"media-col-default",MEDIA_DEFS[0],PRINTER_PAPER_MARGIN);
  }
  if (f.want("media-col-ready",true)) {
    bool first=true;
    for(size_t i=0;i<MEDIA_COUNT;++i){
      if(!MEDIA_DEFS[i].ready) continue;
      writeMediaCol(w,first?"media-col-ready":nullptr,MEDIA_DEFS[i],-1);
      first=false;
    }
  }
  if (f.want("media-col-database",true)) {
    bool first=true;
    for(size_t i=0;i<MEDIA_COUNT;++i){
      writeMediaCol(w,first?"media-col-database":nullptr,MEDIA_DEFS[i],PRINTER_PAPER_MARGIN);
      first=false;
#if PRINTER_BORDERLESS
      // Borderless is a supported margin value, so list a zero-margin entry
      // alongside the normal one for media that can print edge to edge.
      if(MEDIA_DEFS[i].borderless) writeMediaCol(w,nullptr,MEDIA_DEFS[i],0);
#endif
    }
  }
  if (f.want("media-source-supported",true)) w.str(0x44,"media-source-supported","main");
  if (f.want("media-type-supported",true)) {
    w.str(0x44,"media-type-supported","stationery");
    w.str(0x44,nullptr,"photographic");
  }
  // The HP plugin asks for these four attributes to decide whether the print
  // dialog may offer borderless output; a zero value is what enables it.
  {
    const char *margins[4]={"media-left-margin-supported","media-right-margin-supported",
                            "media-top-margin-supported","media-bottom-margin-supported"};
    for(uint8_t i=0;i<4;++i){
      if(!f.want(margins[i],true)) continue;
      w.integer(0x21,margins[i],PRINTER_PAPER_MARGIN);
#if PRINTER_BORDERLESS
      w.integer(0x21,nullptr,0);
#endif
    }
  }

  if (f.want("print-scaling-supported",true)) {
    w.str(0x44,"print-scaling-supported","auto");
    w.str(0x44,nullptr,"auto-fit");
    w.str(0x44,nullptr,"fill");
    w.str(0x44,nullptr,"fit");
    w.str(0x44,nullptr,"none");
  }
  if (f.want("print-scaling-default",true)) w.str(0x44,"print-scaling-default","auto");

  if (f.want("output-bin-supported",true)) w.str(0x44,"output-bin-supported","face-up");
  if (f.want("output-bin-default",true)) w.str(0x44,"output-bin-default","face-up");

  if (f.want("sides-supported",true)) {
    w.str(0x44,"sides-supported","one-sided");
#if PRINTER_DUPLEX
    // The 520/540 has no duplexer: HP calls this "manual two-sided".  The
    // client's driver has to print the second side after the user flips the
    // sheet; the bridge only forwards PCL.  manual-duplex-supported tells an
    // HP-aware client which kind of duplex this is.
    w.str(0x44,nullptr,"two-sided-long-edge");
    w.str(0x44,nullptr,"two-sided-short-edge");
#endif
  }
  if (f.want("sides-default",true)) w.str(0x44,"sides-default","one-sided");
#if PRINTER_DUPLEX
  if (f.want("manual-duplex-supported")) w.boolean("manual-duplex-supported",true);
#endif

  if (f.want("copies-supported",true)) w.range("copies-supported",1,1);
  if (f.want("copies-default",true)) w.integer(0x21,"copies-default",1);

  if (f.want("finishings-supported",true)) w.integer(0x23,"finishings-supported",3);
  if (f.want("finishings-default",true)) w.integer(0x23,"finishings-default",3);

  if (f.want("orientation-requested-supported",true)) {
    w.integer(0x23,"orientation-requested-supported",3);
    w.integer(0x23,nullptr,4);
    w.integer(0x23,nullptr,5);
    w.integer(0x23,nullptr,6);
  }
  if (f.want("orientation-requested-default",true)) w.integer(0x23,"orientation-requested-default",3);

  if (f.want("color-supported")) w.boolean("color-supported",true);
  if (f.want("print-color-mode-supported",true)) {
    w.str(0x44,"print-color-mode-supported","auto");
    w.str(0x44,nullptr,"color");
    w.str(0x44,nullptr,"monochrome");
    // HP advertises process-monochrome as well; it tells a driver that a
    // monochrome job still needs the colour pipeline, which is true here
    // because the client rasterises and we forward the result unchanged.
    w.str(0x44,nullptr,"process-monochrome");
  }
  // HP's default is auto, not color: the document decides unless the user
  // explicitly picks monochrome (or a colour document forces colour).
  if (f.want("print-color-mode-default",true)) w.str(0x44,"print-color-mode-default","auto");
  if (f.want("output-mode-supported",true)) {
    w.str(0x44,"output-mode-supported","auto");
    w.str(0x44,nullptr,"color");
    w.str(0x44,nullptr,"monochrome");
  }
  if (f.want("output-mode-default",true)) w.str(0x44,"output-mode-default","auto");

  w.b(0x03);
  if (!w.ok) {
    Log.println("[IPP] Get-Printer-Attributes response overflowed; sending server-error-internal-error");
    sendSimple(client,major,minor,0x0500,requestId,keepAlive);
    return;
  }
  sendHttpIpp(client,w,keepAlive);
}

bool readU16(BodyReader &r,uint16_t &v){
  uint8_t b[2]; if(!r.readExact(b,2)) return false; v=((uint16_t)b[0]<<8)|b[1]; return true;
}

bool parseAttributes(BodyReader &r,RequestAttrs &attrs){
  String currentName;
  uint8_t collectionDepth = 0;
  while(true){
    uint8_t tag=0;
    if(!r.readByte(tag)) return false;
    if(tag==0x03) return true;
    if(tag<=0x0F){ currentName=""; continue; }

    uint16_t nameLen=0,valueLen=0;
    if(!readU16(r,nameLen)) return false;
    String name;
    for(uint16_t i=0;i<nameLen;++i){ uint8_t b=0; if(!r.readByte(b)) return false; name+=(char)b; }
    if(!readU16(r,valueLen)) return false;

    // Collection framing (0x34 / 0x37) and member names (0x4A) are not
    // attributes and must not touch currentName.  Their payload still has to
    // be consumed or the stream desynchronises.
    if(tag==0x4A || tag==0x34 || tag==0x37){
      if(tag==0x34) ++collectionDepth;
      else if(tag==0x37 && collectionDepth) --collectionDepth;
      for(uint16_t i=0;i<valueLen;++i){ uint8_t b=0; if(!r.readByte(b)) return false; }
      continue;
    }

    if(nameLen) currentName=name;

    // A multi-valued attribute repeats with a zero-length name, so the last
    // named attribute still applies.  Only the first value used to be kept,
    // which silently dropped every name after the first in requested-attributes.
    const bool topLevel = (collectionDepth == 0);
    const bool keepFormat = topLevel && currentName=="document-format";
    const bool keepJobId = topLevel && currentName=="job-id" && valueLen==4;
    const bool keepJobUri = topLevel && currentName=="job-uri" && valueLen<256;
    const bool keepJobName = topLevel && currentName=="job-name" && valueLen<128;
    const bool keepRequested = topLevel && currentName=="requested-attributes" && valueLen>0 && valueLen<64;
    const bool keepLastDoc = topLevel && currentName=="last-document" && valueLen==1;
    const bool keep = keepFormat||keepJobId||keepJobUri||keepJobName||keepRequested||keepLastDoc;
    String value;
    if(keep && valueLen<256) value.reserve(valueLen);
    for(uint16_t i=0;i<valueLen;++i){ uint8_t b=0; if(!r.readByte(b)) return false; if(keep && valueLen<256) value+=(char)b; }

    if(keepFormat) attrs.documentFormat=value;
    if(keepJobId){
      attrs.hasJobId=true;
      const uint8_t *p=(const uint8_t*)value.c_str();
      attrs.jobId=((int32_t)p[0]<<24)|((int32_t)p[1]<<16)|((int32_t)p[2]<<8)|(int32_t)p[3];
    }
    if(keepJobUri){ attrs.hasJobUri=true; attrs.jobUri=value; }
    if(keepJobName) attrs.jobName=value;
    if(keepLastDoc){ attrs.hasLastDocument=true; attrs.lastDocument=(value[0]!=0); }
    if(keepRequested && attrs.requestedCount<RequestAttrs::MAX_REQUESTED){
      attrs.requested[attrs.requestedCount++]=value;
    }
  }
}
} // namespace


String IppPcl3Service::printerUuidBare() {
  static String cached;
  if (cached.length()) return cached;
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  uint8_t b[16] = {0x48, 0x50, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x12, 0x34};
  memcpy(b + 2, mac, 6);
  // Deterministic tail from the MAC so the UUID is stable across reboot/OTA.
  b[8] = mac[0] ^ 0xA5;
  b[9] = mac[1] ^ 0x5A;
  b[10] = mac[2] ^ 0x3C;
  b[11] = mac[3] ^ 0xC3;
  b[12] = mac[4] ^ 0x0F;
  b[13] = mac[5] ^ 0xF0;
  b[6] = (b[6] & 0x0F) | 0x40;
  b[8] = (b[8] & 0x3F) | 0x80;
  char out[40];
  snprintf(out, sizeof(out),
           "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
           b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
           b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
  cached = out;
  return cached;
}

String printerUuid() {
  return String("urn:uuid:") + IppPcl3Service::printerUuidBare();
}

void IppPcl3Service::refreshJobState(){
  if (lastJobId_ == 0) return;
  // A Create-Job'd job stays pending until its first document byte arrives.
  if (lastJobState_ == 3 && !jobStartedProcessing_) return;
  if (lastJobState_ != 3 && lastJobState_ != 4) return;

  const auto state = printer_.state();
  if (state == UsbPrinterBackend::IDLE) {
    lastJobState_ = 9;
    lastJobReason_ = "job-completed-successfully";
    jobCompletedUpTime_ = upTimeSeconds();
    pendingJob_ = false;
    Log.printf("[IPP] Job %lu state -> 9 completed\n", (unsigned long)lastJobId_);
  } else if (state == UsbPrinterBackend::ERROR) {
    lastJobState_ = 8;
    lastJobReason_ = "aborted-by-system";
    pendingJob_ = false;
    Log.printf("[IPP] Job %lu state -> 8 aborted: %s\n",
               (unsigned long)lastJobId_, printer_.statusReason().c_str());
  } else if (state == UsbPrinterBackend::OFFLINE) {
    lastJobState_ = 6;
    lastJobReason_ = "printer-stopped";
    pendingJob_ = false;
    Log.printf("[IPP] Job %lu state -> 6 stopped: %s\n",
               (unsigned long)lastJobId_, printer_.statusReason().c_str());
  }
}

void IppPcl3Service::trackPrinterState(){
  const uint8_t s = (uint8_t)printer_.state();
  if (s != observedPrinterState_) {
    observedPrinterState_ = s;
    stateChangeUpTime_ = upTimeSeconds();
  }
}

void IppPcl3Service::begin(){
  if(!bodyBuffer){
    bodyBuffer=(uint8_t*)heap_caps_malloc(BODY_BUFFER,MALLOC_CAP_SPIRAM);
    if(!bodyBuffer){ Log.println("[IPP] PSRAM alloc failed for IPP document buffer, using heap"); bodyBuffer=(uint8_t*)malloc(BODY_BUFFER); }
    if(!bodyBuffer){ Log.println("[IPP] Failed to allocate IPP document buffer; IPP disabled"); return; }
    Log.printf("[IPP] Allocated %u byte IPP document buffer\n",(unsigned)BODY_BUFFER);
  }
  if (observedPrinterState_ == 0xFF) {
    stateChangeUpTime_ = upTimeSeconds();
    configChangeUpTime_ = upTimeSeconds();
  }
  if(started_) return;
  ippServer.begin(); ippServer.setNoDelay(true); started_=true;
  Log.printf("[IPP] IPP service listening on TCP %u; document buffer=%u response buffer=%u\n",
             (unsigned)IPP_PORT,(unsigned)BODY_BUFFER,(unsigned)IPP_RESPONSE_MAX);
}

void IppPcl3Service::rebind(){
  ippServer.end();
  started_=false;
  begin();
}

void IppPcl3Service::poll(){
  if(!started_) return;
  refreshJobState();
  trackPrinterState();

  WiFiClient client=ippServer.accept();
  if(client) handleClient(client);
}

void IppPcl3Service::handleClient(WiFiClient &client){
  if(!bodyBuffer){
    lastError_="IPP document buffer unavailable";
    client.stop();
    return;
  }
  clientActive_=true; lastError_=""; client.setNoDelay(true); client.setTimeout(CLIENT_TIMEOUT_MS);
  refreshJobState();

  String header;
  if(!readHttpHeader(client,header)){
    lastError_="HTTP header read failed";
    client.stop(); clientActive_=false; return;
  }

  const String expect=headerValue(header,"Expect");
  if(expect.equalsIgnoreCase("100-continue")){
    client.print("HTTP/1.1 100 Continue\r\n\r\n"); client.flush();
  }

  const bool http11 = header.indexOf("HTTP/1.1") >= 0;
  const String connection = headerValue(header,"Connection");
  const bool keepAliveRequested = http11 && !connection.equalsIgnoreCase("close");
  // HTTP keep-alive is not offered.  The ESP32 core cannot retain an accepted
  // socket by assignment (NetworkClient::operator= is a comparison), so holding
  // one across poll() calls would need a heap-owned client or a blocking loop.
  // Every response therefore says "Connection: close", which is valid HTTP/1.1
  // and is what the previous builds did.  The header is still logged so a
  // client that asks for keep-alive is visible in the diagnostics.
  const bool keep = false;

  const String transfer=headerValue(header,"Transfer-Encoding");
  const String lengthText=headerValue(header,"Content-Length");
  BodyReader body(client);
  body.chunked=transfer.equalsIgnoreCase("chunked");
  body.remaining=body.chunked ? -1 : (lengthText.length()?lengthText.toInt():-1);

  uint8_t hdr[8];
  if(!body.readExact(hdr,sizeof(hdr))){
    lastError_="truncated IPP header";
    client.stop(); clientActive_=false; return;
  }

  const uint8_t major=hdr[0],minor=hdr[1];
  const uint16_t op=((uint16_t)hdr[2]<<8)|hdr[3];
  const uint32_t requestId=((uint32_t)hdr[4]<<24)|((uint32_t)hdr[5]<<16)|((uint32_t)hdr[6]<<8)|hdr[7];

  RequestAttrs attrs;
  if(!parseAttributes(body,attrs)){
    lastError_="malformed IPP attributes";
    sendSimple(client,major,minor,0x0400,requestId,false);
    client.stop(); clientActive_=false; return;
  }
  const String &format = attrs.documentFormat;

  Log.printf("[IPP] op=0x%04X request-id=%lu format=%s transfer=%s keep-alive=%s\n",
                op,(unsigned long)requestId,format.c_str(),body.chunked?"chunked":"fixed",
                keepAliveRequested?"yes":"no");

  const bool documentOp = (op==0x0002 || op==0x0006);

  // Consume the (normally empty) remainder of a non-document request so the
  // socket is closed with a clean FIN instead of an RST from unread data.
  // Document ops consume their own body inside streamDocument().
  if (!documentOp && (body.chunked || body.remaining > 0)) {
    uint8_t scratch[64];
    const unsigned long drainStart = millis();
    while (!body.done && !body.framingError) {
      if (!body.readBlock(scratch,sizeof(scratch))) break;
      if (millis() - drainStart > 2000) break;
      esp_task_wdt_reset();
    }
  }

  auto streamDocument = [&](BodyReader &b, uint32_t jobId) -> bool {
    String error;
    const uint32_t streamStarted = millis();
    uint32_t networkReadMs = 0, usbSendMs = 0;
    const unsigned long deadline = millis() + IPP_JOB_DEADLINE_MS;
    bool ok = true;

    while (true) {
      esp_task_wdt_reset();
      if ((long)(millis() - deadline) >= 0) {
        lastError_ = "job exceeded the document deadline";
        ok = false;
        break;
      }
      const uint32_t readStarted = millis();
      const size_t n = b.readBlock(bodyBuffer, BODY_BUFFER);
      networkReadMs += millis() - readStarted;
      if (!n) break;

      const uint32_t usbStarted = millis();
      const bool sent = printer_.sendDirect(bodyBuffer, n, error);
      usbSendMs += millis() - usbStarted;
      if (!sent) { lastError_ = error; ok = false; break; }

      if (!jobStartedProcessing_) {
        jobStartedProcessing_ = true;
        lastJobState_ = 4;
        lastJobReason_ = "job-printing";
        jobProcessingUpTime_ = upTimeSeconds();
      }
      lastJobBytes_ += n;
      yield();
    }

    const uint32_t totalMs = millis() - streamStarted;
    const uint64_t kibPerSecond = totalMs ? ((lastJobBytes_ * 1000ULL) / 1024ULL / totalMs) : 0;
    Log.printf("[IPP][PERF] job=%lu bytes=%llu total=%lu ms net-read=%lu ms usb-send=%lu ms avg=%llu KiB/s buffer=%u\n",
                  (unsigned long)jobId,
                  (unsigned long long)lastJobBytes_,
                  (unsigned long)totalMs,
                  (unsigned long)networkReadMs,
                  (unsigned long)usbSendMs,
                  (unsigned long long)kibPerSecond,
                  (unsigned)BODY_BUFFER);

    const bool complete = b.done || (b.peerClosed && b.atChunkBoundary());
    if (ok && complete && !b.framingError && lastJobBytes_ > 0) {
      if (!b.done) {
        Log.printf("[IPP] job=%lu: client closed at a chunk boundary without the terminating 0-chunk; accepting the document\n",
                   (unsigned long)jobId);
      }
      return true;
    }

    if (lastError_.isEmpty()) {
      if (b.framingError) lastError_="HTTP chunk framing error";
      else if (b.peerClosed) lastError_="IPP client disconnected before the complete document was received";
      else if (lastJobBytes_ == 0) lastError_="document was empty";
      else lastError_="document body ended unexpectedly";
    }
    Log.printf("[IPP] document failed: job=%lu bytes=%llu done=%s peer-closed=%s framing-error=%s reason=%s\n",
                  (unsigned long)jobId,(unsigned long long)lastJobBytes_,
                  b.done?"yes":"no",b.peerClosed?"yes":"no",b.framingError?"yes":"no",
                  lastError_.c_str());
    if (jobStartedProcessing_ || lastJobBytes_ > 0) printer_.abortRawJob(lastError_);
    lastJobState_ = 8;
    lastJobReason_ = "aborted-by-system";
    return false;
  };

  if(op==0x000B){
    // --- Get-Printer-Attributes ---
    PrinterSnapshot snap;
    snap.upTime = upTimeSeconds();
    snap.stateChangeUp = stateChangeUpTime_;
    snap.configChangeUp = configChangeUpTime_;
    snap.queued = (lastJobState_==3||lastJobState_==4) ? 1 : 0;

    const bool paperEmpty = printer_.usbStatusValid() && printer_.usbPaperEmpty();
    const bool statusErr  = printer_.usbStatusValid() && printer_.usbStatusError();
    const bool selected   = printer_.usbStatusValid() && printer_.usbStatusSelected();
    const auto st = printer_.state();

    snap.accepting = (st == UsbPrinterBackend::IDLE || st == UsbPrinterBackend::PRINTING);
    if (st == UsbPrinterBackend::IDLE) {
      snap.ippState = 3;
      snap.reasons = "none";
    } else if (st == UsbPrinterBackend::PRINTING) {
      snap.ippState = 4;
      snap.reasons = "none";
    } else {
      snap.ippState = 5;  // stopped
      if (paperEmpty) snap.reasons = "media-empty-error";
      else if (st == UsbPrinterBackend::OFFLINE && printer_.usbStatusValid() && !selected) snap.reasons = "offline";
      else if (st == UsbPrinterBackend::ERROR || statusErr) snap.reasons = "other-error";
      else snap.reasons = "offline";
    }

    if (paperEmpty) {
      snap.alertCode = "mediaEmpty";
      snap.alertSeverity = "critical";
      snap.alertGroup = "mediaPaths";
      snap.alertDescription = "mediaEmpty";
    } else if (st == UsbPrinterBackend::ERROR || statusErr) {
      snap.alertCode = "otherError";
      snap.alertSeverity = "critical";
      snap.alertGroup = "generalPrinter";
      snap.alertDescription = "otherError";
    } else if (st == UsbPrinterBackend::PRINTING) {
      snap.alertCode = "processing";
      snap.alertSeverity = "other";
      snap.alertGroup = "generalPrinter";
      snap.alertDescription = "processing";
    } else {
      snap.alertCode = "printerReadyToPrint";
      snap.alertSeverity = "other";
      snap.alertGroup = "generalPrinter";
      snap.alertDescription = "ready";
    }

    sendPrinterAttributes(client,major,minor,requestId,attrs,snap,keep);
  } else if(op==0x0004){
    // --- Validate-Job ---
    sendSimple(client,major,minor,(format.isEmpty()||supportedDocumentFormat(format))?0x0000:0x040A,requestId,keep);
  } else if(op==0x0002){
    // --- Print-Job ---
    if(!format.isEmpty() && !supportedDocumentFormat(format)){
      lastError_="Print-Job rejected: unsupported document-format";
      sendSimple(client,major,minor,0x040A,requestId,false);
    } else if(printer_.rawClientConnected()){
      // 0x0503 server-error-busy makes the HP Print Service retry this job
      // shortly; 0x0507 (not-accepting-jobs) would instead mark the printer
      // stopped and make Android remove/re-add it.
      lastError_="Print-Job refused: RAW 9100 client holds the USB stream";
      sendSimple(client,major,minor,0x0503,requestId,false);
    } else if(printer_.state()==UsbPrinterBackend::PRINTING){
      // We have no job queue, so the previous job is still draining.  Report
      // busy (retryable), never not-accepting-jobs, otherwise Android marks the
      // printer stopped even though printer-is-accepting-jobs is true.
      lastError_="Print-Job refused: printer is still printing the previous job";
      sendSimple(client,major,minor,0x0503,requestId,false);
    } else if(!printer_.online()){
      lastError_="Printer unavailable";
      sendSimple(client,major,minor,0x0507,requestId,false);
    } else {
      const uint32_t jobId = nextJobId_++;
      lastJobId_=jobId; lastJobState_=3; lastJobReason_="job-incoming";
      lastJobBytes_=0; jobStartedProcessing_=false; lastJobName_=attrs.jobName;
      jobProcessingUpTime_=0; jobCompletedUpTime_=0; pendingJob_=false;

      const bool ok = streamDocument(body,jobId);
      sendJobResponse(client,major,minor,ok?0x0000:0x0500,requestId,jobId,
                      lastJobState_,lastJobReason_,lastJobName_,upTimeSeconds(),
                      jobProcessingUpTime_,jobCompletedUpTime_,false);
      if(ok){
        printer_.finishRawJob();
        Log.printf("[IPP] Print-Job accepted: job=%lu %llu bytes\n",
                      (unsigned long)jobId,(unsigned long long)lastJobBytes_);
      }
    }
  } else if(op==0x0005){
    // --- Create-Job: allocate the job, the document follows in Send-Document ---
    if(printer_.state()==UsbPrinterBackend::PRINTING){
      lastError_="Create-Job refused: printer is still printing the previous job";
      sendSimple(client,major,minor,0x0503,requestId,false);
    } else if(!printer_.online()){
      lastError_="Create-Job refused: printer unavailable";
      sendSimple(client,major,minor,0x0507,requestId,false);
    } else {
      const uint32_t jobId = nextJobId_++;
      lastJobId_=jobId; lastJobState_=3; lastJobReason_="job-incoming";
      lastJobBytes_=0; jobStartedProcessing_=false; lastJobName_=attrs.jobName;
      jobProcessingUpTime_=0; jobCompletedUpTime_=0; pendingJob_=true;
      Log.printf("[IPP] Create-Job: job=%lu awaiting Send-Document\n",(unsigned long)jobId);
      sendJobResponse(client,major,minor,0x0000,requestId,jobId,lastJobState_,lastJobReason_,
                      lastJobName_,upTimeSeconds(),0,0,keep);
    }
  } else if(op==0x0006){
    // --- Send-Document ---
    bool uriOk = true;
    if(attrs.hasJobUri && attrs.jobUri.length()){
      uriOk = attrs.jobUri.endsWith(String("/job/")+String(lastJobId_));
    }
    if(!pendingJob_ || lastJobId_==0){
      lastError_="Send-Document: no job awaiting a document";
      sendSimple(client,major,minor,0x0406,requestId,false);
    } else if(attrs.hasJobId && (uint32_t)attrs.jobId != lastJobId_){
      lastError_="Send-Document: job-id does not match";
      sendSimple(client,major,minor,0x0406,requestId,false);
    } else if(!uriOk){
      lastError_="Send-Document: job-uri does not match";
      sendSimple(client,major,minor,0x0406,requestId,false);
    } else if(!format.isEmpty() && !supportedDocumentFormat(format)){
      lastError_="Send-Document rejected: unsupported document-format";
      sendSimple(client,major,minor,0x040A,requestId,false);
    } else {
      lastJobBytes_=0; jobStartedProcessing_=false;
      const bool ok = streamDocument(body,lastJobId_);
      pendingJob_=false;
      sendJobResponse(client,major,minor,ok?0x0000:0x0500,requestId,lastJobId_,
                      lastJobState_,lastJobReason_,lastJobName_,upTimeSeconds(),
                      jobProcessingUpTime_,jobCompletedUpTime_,false);
      if(ok){
        printer_.finishRawJob();
        Log.printf("[IPP] Send-Document accepted: job=%lu %llu bytes\n",
                      (unsigned long)lastJobId_,(unsigned long long)lastJobBytes_);
      }
    }
  } else if(op==0x0008){
    // --- Cancel-Job ---
    if(!attrs.hasJobId && !attrs.hasJobUri){
      sendSimple(client,major,minor,0x0400,requestId,keep);
    } else if(lastJobId_==0 || (attrs.hasJobId && (uint32_t)attrs.jobId != lastJobId_)){
      sendSimple(client,major,minor,0x0406,requestId,keep);
    } else if(lastJobState_==7 || lastJobState_==8 || lastJobState_==9){
      sendSimple(client,major,minor,0x0404,requestId,keep);
    } else {
      if(printer_.state()==UsbPrinterBackend::PRINTING) printer_.abortRawJob("job-canceled-by-user");
      pendingJob_=false;
      lastJobState_=7;
      lastJobReason_="job-canceled-by-user";
      Log.printf("[IPP] Job %lu canceled by client\n",(unsigned long)lastJobId_);
      sendJobResponse(client,major,minor,0x0000,requestId,lastJobId_,lastJobState_,lastJobReason_,
                      lastJobName_,upTimeSeconds(),jobProcessingUpTime_,jobCompletedUpTime_,keep);
    }
  } else if(op==0x003B){
    // --- Close-Job ---
    if(lastJobId_==0){
      sendSimple(client,major,minor,0x0406,requestId,keep);
    } else {
      if(pendingJob_){
        pendingJob_=false;
        lastJobState_=9;
        lastJobReason_="job-completed-successfully";
        jobCompletedUpTime_=upTimeSeconds();
      }
      sendJobResponse(client,major,minor,0x0000,requestId,lastJobId_,lastJobState_,lastJobReason_,
                      lastJobName_,upTimeSeconds(),jobProcessingUpTime_,jobCompletedUpTime_,keep);
    }
  } else if(op==0x0009 || op==0x000A){
    // --- Get-Job-Attributes / Get-Jobs ---
    refreshJobState();
    if(attrs.hasJobId && lastJobId_ && (uint32_t)attrs.jobId != lastJobId_){
      sendSimple(client,major,minor,0x0406,requestId,keep);
    } else if(lastJobId_){
      Log.printf("[IPP] Job status: id=%lu state=%u reason=%s\n",
                    (unsigned long)lastJobId_,lastJobState_,lastJobReason_.c_str());
      sendJobResponse(client,major,minor,0x0000,requestId,lastJobId_,lastJobState_,lastJobReason_,
                      lastJobName_,upTimeSeconds(),jobProcessingUpTime_,jobCompletedUpTime_,keep);
    } else {
      sendSimple(client,major,minor,0x0000,requestId,keep);
    }
  } else {
    sendSimple(client,major,minor,0x0501,requestId,keep);
  }

  client.stop();
  clientActive_=false;
}
