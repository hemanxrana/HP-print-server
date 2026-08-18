#include "mobile_ipp_server.h"
#include "mobile_print_profile.h"
#include <stdlib.h>
#include <string.h>

namespace {
constexpr size_t MAX_IPP_BODY = 4 * 1024 * 1024;
constexpr size_t RESPONSE_CAPACITY = 8192;
constexpr uint16_t OP_PRINT_JOB = 0x0002;
constexpr uint16_t OP_VALIDATE_JOB = 0x0004;
constexpr uint16_t OP_CANCEL_JOB = 0x0008;
constexpr uint16_t OP_GET_JOB_ATTRIBUTES = 0x0009;
constexpr uint16_t OP_GET_JOBS = 0x000A;
constexpr uint16_t OP_GET_PRINTER_ATTRIBUTES = 0x000B;
constexpr uint16_t ST_OK = 0x0000;
constexpr uint16_t ST_BAD_REQUEST = 0x0400;
constexpr uint16_t ST_NOT_POSSIBLE = 0x0403;
constexpr uint16_t ST_NOT_FOUND = 0x0406;
constexpr uint16_t ST_DOC_FORMAT = 0x040B;
constexpr uint16_t ST_UNSUPPORTED = 0x0501;
constexpr uint16_t ST_UNAVAILABLE = 0x0502;

uint16_t u16(const uint8_t *p) { return ((uint16_t)p[0] << 8) | p[1]; }
uint32_t u32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
void put16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

bool attr(uint8_t *o, size_t cap, size_t &p, uint8_t tag, const char *name, const uint8_t *value, size_t len) {
  const size_t nl = strlen(name);
  if (nl > 65535 || len > 65535 || p + 5 + nl + len > cap) return false;
  o[p++] = tag; put16(o + p, (uint16_t)nl); p += 2;
  memcpy(o + p, name, nl); p += nl; put16(o + p, (uint16_t)len); p += 2;
  if (len) memcpy(o + p, value, len); p += len;
  return true;
}

bool text(uint8_t *o, size_t cap, size_t &p, uint8_t tag, const char *name, const String &value) {
  return attr(o, cap, p, tag, name, (const uint8_t *)value.c_str(), value.length());
}
bool keyword(uint8_t *o, size_t cap, size_t &p, const char *name, const char *value) {
  return attr(o, cap, p, 0x44, name, (const uint8_t *)value, strlen(value));
}
bool enumAttr(uint8_t *o, size_t cap, size_t &p, const char *name, uint32_t value) {
  uint8_t b[4]; put32(b, value); return attr(o, cap, p, 0x23, name, b, 4);
}
bool integerAttr(uint8_t *o, size_t cap, size_t &p, const char *name, uint32_t value) {
  uint8_t b[4]; put32(b, value); return attr(o, cap, p, 0x21, name, b, 4);
}
bool boolAttr(uint8_t *o, size_t cap, size_t &p, const char *name, bool value) {
  uint8_t b = value ? 1 : 0; return attr(o, cap, p, 0x22, name, &b, 1);
}
bool rangeAttr(uint8_t *o, size_t cap, size_t &p, const char *name, uint32_t lo, uint32_t hi) {
  uint8_t b[8]; put32(b, lo); put32(b + 4, hi); return attr(o, cap, p, 0x33, name, b, 8);
}

bool pcl3Gui(const String &format) {
  String f = format; f.trim(); f.toLowerCase();
  return f == "application/vnd.hp-pcl" || f == "application/vnd.hp-pcl3gui";
}

bool requested(const String &list, const char *name) {
  if (list.isEmpty() || list == "all") return true;
  String n = name; n.toLowerCase();
  int start = 0;
  while (start < (int)list.length()) {
    int end = list.indexOf(',', start); if (end < 0) end = list.length();
    String item = list.substring(start, end); item.trim(); item.toLowerCase();
    if (item == n) return true;
    start = end + 1;
  }
  return false;
}

bool readLine(WiFiClient &c, String &line, unsigned long deadline) {
  while (millis() < deadline) {
    if (c.available()) { line = c.readStringUntil('\n'); line.trim(); return true; }
    delay(1);
  }
  return false;
}

bool readChunkedBody(WiFiClient &c, uint8_t *body, size_t cap, size_t &length) {
  length = 0;
  while (true) {
    String line;
    if (!readLine(c, line, millis() + 10000)) return false;
    int semi = line.indexOf(';'); if (semi >= 0) line = line.substring(0, semi);
    const size_t chunk = strtoul(line.c_str(), nullptr, 16);
    if (chunk == 0) {
      do { if (!readLine(c, line, millis() + 5000)) return false; } while (!line.isEmpty());
      return true;
    }
    if (chunk > cap - length) return false;
    size_t got = 0;
    while (got < chunk) {
      if (!c.available()) { delay(1); continue; }
      int n = c.read(body + length + got, chunk - got); if (n <= 0) return false; got += (size_t)n;
    }
    length += chunk;
    unsigned long deadline = millis() + 5000;
    while (c.available() < 2 && millis() < deadline) delay(1);
    if (c.read() != '\r' || c.read() != '\n') return false;
  }
}
}

MobileIppServer::MobileIppServer(uint16_t port) : server_(port), port_(port) {}

void MobileIppServer::begin(const String &name, const String &uri, JobHandler handler, MobilePrintQueue *queue) {
  printerName_ = name; printerUri_ = uri; handler_ = handler; queue_ = queue;
  const int scheme = uri.indexOf("://");
  const int slash = scheme >= 0 ? uri.indexOf('/', scheme + 3) : -1;
  printerPath_ = slash >= 0 ? uri.substring(slash) : String("/ipp/print");
  server_.begin(); running_ = true;
  Serial.printf("[IPP] Listening on TCP %u at %s\n", port_, printerUri_.c_str());
}

bool MobileIppServer::readHttpBody(WiFiClient &c, uint8_t **body, size_t &length) {
  *body = nullptr; length = 0; c.setTimeout(5);
  String line;
  if (!readLine(c, line, millis() + 5000) || !line.startsWith("POST ")) return false;
  const int sp = line.indexOf(' ', 5); if (sp < 0) return false;
  String target = line.substring(5, sp); const int q = target.indexOf('?'); if (q >= 0) target = target.substring(0, q);
  if (target != printerPath_ && target != printerPath_ + "/" && target != "/ipp/print" && target != "/ipp/print/") return false;

  size_t contentLength = 0; bool haveLength = false, chunked = false, isIpp = false, expect = false;
  const unsigned long deadline = millis() + 5000;
  while (readLine(c, line, deadline)) {
    if (line.isEmpty()) break;
    String h = line; h.toLowerCase();
    if (h.startsWith("content-length:")) { String v = h.substring(15); v.trim(); contentLength = strtoul(v.c_str(), nullptr, 10); haveLength = true; }
    else if (h.startsWith("content-type:")) isIpp = h.indexOf("application/ipp") >= 0;
    else if (h.startsWith("transfer-encoding:")) chunked = h.indexOf("chunked") >= 0;
    else if (h.startsWith("expect:")) expect = h.indexOf("100-continue") >= 0;
  }
  if (!isIpp || (!haveLength && !chunked) || contentLength > MAX_IPP_BODY) {
    Serial.printf("[IPP] Bad HTTP request: ipp=%d length=%d chunked=%d cl=%u\n", isIpp, haveLength, chunked, (unsigned)contentLength);
    return false;
  }
  if (expect) c.print("HTTP/1.1 100 Continue\r\n\r\n");

  uint8_t *b = (uint8_t *)ps_malloc(MAX_IPP_BODY); if (!b) b = (uint8_t *)malloc(MAX_IPP_BODY); if (!b) return false;
  size_t got = 0; bool ok = true;
  if (chunked) ok = readChunkedBody(c, b, MAX_IPP_BODY, got);
  else {
    const unsigned long bodyDeadline = millis() + 30000;
    while (got < contentLength && millis() < bodyDeadline) {
      if (!c.available()) { delay(1); continue; }
      int n = c.read(b + got, contentLength - got); if (n > 0) got += (size_t)n;
    }
    ok = got == contentLength;
  }
  if (!ok || got < 8) { free(b); Serial.printf("[IPP] Body read failed: got=%u\n", (unsigned)got); return false; }
  *body = b; length = got; return true;
}

bool MobileIppServer::buildResponse(const uint8_t *rq, size_t len, uint8_t *out, size_t cap, size_t &rl) {
  rl = 0; if (len < 8 || cap < 64) return false;
  uint16_t version = u16(rq); if (version != 0x0100 && version != 0x0101 && version != 0x0200) version = 0x0101;
  const uint16_t op = u16(rq + 2); const uint32_t requestId = u32(rq + 4);
  String format = MobilePrintProfile::FORMAT_PCL3GUI, requestedAttrs, whichJobs = "not-completed";
  uint32_t requestedJob = 0; size_t documentOffset = len; bool sawOperationGroup = false; String lastName; size_t p = 8;

  while (p < len) {
    const uint8_t tag = rq[p++];
    if (tag == 0x03) { documentOffset = p; break; }
    if (tag == 0x01) { sawOperationGroup = true; continue; }
    if (tag == 0x02 || tag == 0x04 || tag == 0x05) continue;
    if (p + 4 > len) return false;
    const uint16_t nl = u16(rq + p); p += 2; if (p + nl + 2 > len) return false;
    String name; for (uint16_t i = 0; i < nl; ++i) name += (char)rq[p + i]; if (nl) lastName = name; else name = lastName; p += nl;
    const uint16_t vl = u16(rq + p); p += 2; if (p + vl > len) return false;
    if (name == "document-format" && vl < 256) { format = ""; for (uint16_t i = 0; i < vl; ++i) format += (char)rq[p + i]; }
    else if (name == "requested-attributes" && vl < 1024) { String v; for (uint16_t i = 0; i < vl; ++i) v += (char)rq[p + i]; if (requestedAttrs.length()) requestedAttrs += ','; requestedAttrs += v; }
    else if (name == "which-jobs" && vl < 64) { whichJobs = ""; for (uint16_t i = 0; i < vl; ++i) whichJobs += (char)rq[p + i]; }
    else if (name == "job-id" && vl == 4) requestedJob = u32(rq + p);
    p += vl;
  }
  if (!sawOperationGroup) return false;
  if (op == OP_GET_JOB_ATTRIBUTES && requestedJob == 0) return false;
  if (op == OP_GET_JOBS && requestedAttrs.isEmpty()) requestedAttrs = "job-id,job-uri,job-state,job-state-reasons,job-name,document-format";

  uint16_t status = ST_OK; String statusMessage; uint32_t jobId = 0;
  if (op == OP_PRINT_JOB) {
    if (documentOffset >= len) { status = ST_BAD_REQUEST; statusMessage = "Print-Job requires a PCL 3 GUI document"; }
    else if (!pcl3Gui(format)) { status = ST_DOC_FORMAT; statusMessage = "Only application/vnd.hp-PCL (HP PCL 3 GUI) is supported"; }
    else if (!handler_) { status = ST_UNAVAILABLE; statusMessage = "Print backend unavailable"; }
    else if (!handler_(rq + documentOffset, len - documentOffset, format, jobId, statusMessage)) { status = ST_NOT_POSSIBLE; if (statusMessage.isEmpty()) statusMessage = "Document rejected by print backend"; }
  } else if (op == OP_VALIDATE_JOB) {
    if (documentOffset < len) { status = ST_BAD_REQUEST; statusMessage = "Validate-Job must not contain document data"; }
    else if (!pcl3Gui(format)) { status = ST_DOC_FORMAT; statusMessage = "Only HP PCL 3 GUI is supported"; }
  } else if (op == OP_CANCEL_JOB) {
    MobilePrintQueue::JobInfo info;
    if (!queue_ || requestedJob == 0 || !queue_->getJob(requestedJob, info)) { status = ST_NOT_FOUND; statusMessage = "Job not found"; }
    else { String e; if (!queue_->cancel(requestedJob, e)) { status = ST_NOT_POSSIBLE; statusMessage = e; } }
  } else if (op == OP_GET_JOB_ATTRIBUTES) {
    MobilePrintQueue::JobInfo info; if (!queue_ || !queue_->getJob(requestedJob, info)) { status = ST_NOT_FOUND; statusMessage = "Job not found"; }
  } else if (op != OP_GET_PRINTER_ATTRIBUTES && op != OP_GET_JOBS) { status = ST_UNSUPPORTED; statusMessage = "IPP operation not supported"; }

  size_t w = 0; out[w++] = version >> 8; out[w++] = version; put16(out + w, status); w += 2; put32(out + w, requestId); w += 4; out[w++] = 0x01;
  if (!text(out, cap, w, 0x47, "attributes-charset", "utf-8") || !text(out, cap, w, 0x48, "attributes-natural-language", "en")) return false;
  if (!statusMessage.isEmpty() && !text(out, cap, w, 0x41, "status-message", statusMessage)) return false;

  if (op == OP_GET_PRINTER_ATTRIBUTES && status == ST_OK) {
    out[w++] = 0x04;
    if (requested(requestedAttrs, "printer-name") && !text(out, cap, w, 0x42, "printer-name", printerName_)) return false;
    if (requested(requestedAttrs, "printer-make-and-model") && !text(out, cap, w, 0x42, "printer-make-and-model", "HP Smart Tank 520 - HP PCL 3 GUI")) return false;
    if (requested(requestedAttrs, "printer-info") && !text(out, cap, w, 0x41, "printer-info", "ESP32-S3 USB print server")) return false;
    if (requested(requestedAttrs, "printer-uri-supported") && !text(out, cap, w, 0x45, "printer-uri-supported", printerUri_)) return false;
    if (requested(requestedAttrs, "uri-authentication-supported") && !keyword(out, cap, w, "uri-authentication-supported", "none")) return false;
    if (requested(requestedAttrs, "uri-security-supported") && !keyword(out, cap, w, "uri-security-supported", "none")) return false;
    if (requested(requestedAttrs, "ipp-versions-supported") && !keyword(out, cap, w, "ipp-versions-supported", "1.1")) return false;
    if (requested(requestedAttrs, "document-format-supported") && !attr(out, cap, w, 0x49, "document-format-supported", (const uint8_t *)MobilePrintProfile::FORMAT_PCL3GUI, strlen(MobilePrintProfile::FORMAT_PCL3GUI))) return false;
    if (requested(requestedAttrs, "document-format-default") && !attr(out, cap, w, 0x49, "document-format-default", (const uint8_t *)MobilePrintProfile::FORMAT_PCL3GUI, strlen(MobilePrintProfile::FORMAT_PCL3GUI))) return false;
    if (requested(requestedAttrs, "printer-state") && !enumAttr(out, cap, w, "printer-state", 3)) return false;
    if (requested(requestedAttrs, "printer-state-reasons") && !keyword(out, cap, w, "printer-state-reasons", "none")) return false;
    if (requested(requestedAttrs, "printer-is-accepting-jobs") && !boolAttr(out, cap, w, "printer-is-accepting-jobs", handler_ != nullptr)) return false;
    if (requested(requestedAttrs, "queued-job-count") && !integerAttr(out, cap, w, "queued-job-count", queue_ ? queue_->activeCount() : 0)) return false;
    if (requested(requestedAttrs, "copies-supported") && !rangeAttr(out, cap, w, "copies-supported", 1, 99)) return false;
    if (requested(requestedAttrs, "color-supported") && !boolAttr(out, cap, w, "color-supported", true)) return false;
    if (requested(requestedAttrs, "operations-supported")) {
      if (!enumAttr(out, cap, w, "operations-supported", OP_PRINT_JOB) || !enumAttr(out, cap, w, "operations-supported", OP_VALIDATE_JOB) || !enumAttr(out, cap, w, "operations-supported", OP_CANCEL_JOB) || !enumAttr(out, cap, w, "operations-supported", OP_GET_JOB_ATTRIBUTES) || !enumAttr(out, cap, w, "operations-supported", OP_GET_JOBS) || !enumAttr(out, cap, w, "operations-supported", OP_GET_PRINTER_ATTRIBUTES)) return false;
    }
  } else if (op == OP_PRINT_JOB && status == ST_OK) {
    out[w++] = 0x02; if (!integerAttr(out, cap, w, "job-id", jobId)) return false;
    String jobUri = printerUri_ + "/" + String(jobId); if (!text(out, cap, w, 0x45, "job-uri", jobUri)) return false;
    if (!enumAttr(out, cap, w, "job-state", MobilePrintQueue::STATE_PENDING)) return false;
  } else if (op == OP_GET_JOB_ATTRIBUTES && status == ST_OK) {
    MobilePrintQueue::JobInfo info;
    if (queue_ && queue_->getJob(requestedJob, info)) {
      out[w++] = 0x02; if (!integerAttr(out, cap, w, "job-id", info.id)) return false;
      String jobUri = printerUri_ + "/" + String(info.id); if (!text(out, cap, w, 0x45, "job-uri", jobUri)) return false;
      if (!enumAttr(out, cap, w, "job-state", info.state)) return false;
      if (!keyword(out, cap, w, "job-state-reasons", info.reason.length() ? info.reason.c_str() : "none")) return false;
      if (!attr(out, cap, w, 0x49, "document-format", (const uint8_t *)info.format.c_str(), info.format.length())) return false;
    }
  } else if (op == OP_GET_JOBS && status == ST_OK && queue_) {
    for (uint8_t i = 0; i < queue_->count() && i < MobilePrintQueue::MAX_JOBS; ++i) {
      MobilePrintQueue::JobInfo info; if (!queue_->getJobAt(i, info)) continue;
      if (whichJobs == "completed" && info.state != MobilePrintQueue::STATE_COMPLETED) continue;
      if (whichJobs == "not-completed" && info.state == MobilePrintQueue::STATE_COMPLETED) continue;
      out[w++] = 0x02;
      if (!integerAttr(out, cap, w, "job-id", info.id)) return false;
      String jobUri = printerUri_ + "/" + String(info.id); if (!text(out, cap, w, 0x45, "job-uri", jobUri)) return false;
      if (!enumAttr(out, cap, w, "job-state", info.state)) return false;
      if (!attr(out, cap, w, 0x49, "document-format", (const uint8_t *)info.format.c_str(), info.format.length())) return false;
    }
  }

  if (w + 1 > cap) return false; out[w++] = 0x03; rl = w; return true;
}

void MobileIppServer::handleClient(WiFiClient &client) {
  static uint8_t response[RESPONSE_CAPACITY]; uint8_t *request = nullptr; size_t requestLength = 0;
  const bool httpOk = readHttpBody(client, &request, requestLength); size_t responseLength = 0;
  const bool ippOk = httpOk && buildResponse(request, requestLength, response, sizeof(response), responseLength);
  if (!ippOk) client.print("HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
  else { client.print("HTTP/1.1 200 OK\r\nContent-Type: application/ipp\r\nContent-Length: "); client.print(responseLength); client.print("\r\nConnection: close\r\n\r\n"); client.write(response, responseLength); }
  if (request) free(request); client.stop();
}

void MobileIppServer::poll() { WiFiClient client = server_.available(); if (client) handleClient(client); }
