#include "mobile_ipp_server.h"
#include <stdlib.h>
#include <string.h>

namespace {
constexpr size_t MAX_IPP_BODY = 4 * 1024 * 1024;
constexpr size_t RESPONSE_CAPACITY = 8192;

uint16_t get16(const uint8_t *p) { return ((uint16_t)p[0] << 8) | p[1]; }
void put16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v & 0xff; }
void put32(uint8_t *p, uint32_t v) { p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v; }

bool addString(uint8_t *out, size_t cap, size_t &pos, uint8_t tag,
               const char *name, const String &value) {
  size_t nl = strlen(name), vl = value.length();
  if (nl > 65535 || vl > 65535 || pos + 5 + nl + vl > cap) return false;
  out[pos++] = tag; put16(out + pos, (uint16_t)nl); pos += 2;
  memcpy(out + pos, name, nl); pos += nl;
  put16(out + pos, (uint16_t)vl); pos += 2;
  memcpy(out + pos, value.c_str(), vl); pos += vl;
  return true;
}

bool addInt(uint8_t *out, size_t cap, size_t &pos, uint8_t tag,
            const char *name, uint32_t value) {
  size_t nl = strlen(name);
  if (nl > 65535 || pos + 9 + nl > cap) return false;
  out[pos++] = tag; put16(out + pos, (uint16_t)nl); pos += 2;
  memcpy(out + pos, name, nl); pos += nl;
  put16(out + pos, 4); pos += 2; put32(out + pos, value); pos += 4;
  return true;
}

bool addRange(uint8_t *out, size_t cap, size_t &pos, const char *name, uint32_t low, uint32_t high) {
  size_t nl = strlen(name);
  if (nl > 65535 || pos + 13 + nl > cap) return false;
  out[pos++] = 0x33; put16(out + pos, (uint16_t)nl); pos += 2;
  memcpy(out + pos, name, nl); pos += nl;
  put16(out + pos, 8); pos += 2;
  put32(out + pos, low); pos += 4; put32(out + pos, high); pos += 4;
  return true;
}

bool addKeyword(uint8_t *out, size_t cap, size_t &pos, const char *name, const char *value) {
  return addString(out, cap, pos, 0x44, name, value);
}

bool addMime(uint8_t *out, size_t cap, size_t &pos, const char *value) {
  return addString(out, cap, pos, 0x49, "document-format-supported", value);
}

bool addEnum(uint8_t *out, size_t cap, size_t &pos, const char *name, uint32_t value) {
  return addInt(out, cap, pos, 0x23, name, value);
}

bool addBool(uint8_t *out, size_t cap, size_t &pos, const char *name, bool value) {
  return addInt(out, cap, pos, 0x22, name, value ? 1 : 0);
}
}

MobileIppServer::MobileIppServer(uint16_t port) : server_(port), port_(port) {}

void MobileIppServer::begin(const String &printerName, const String &printerUri, JobHandler handler) {
  printerName_ = printerName;
  printerUri_ = printerUri;
  handler_ = handler;
  server_.begin();
  running_ = true;
  Serial.printf("[IPP] Listening on TCP %u at %s\n", port_, printerUri_.c_str());
}

bool MobileIppServer::readHttpBody(WiFiClient &client, uint8_t **body, size_t &length) {
  *body = nullptr;
  length = 0;
  client.setTimeout(5);
  String requestLine = client.readStringUntil('\n');
  requestLine.trim();
  if (!requestLine.startsWith("POST ")) return false;

  size_t contentLength = 0;
  bool ipp = false;
  unsigned long deadline = millis() + 5000;
  while (millis() < deadline) {
    if (!client.available()) { delay(1); continue; }
    String line = client.readStringUntil('\n');
    line.trim();
    if (line.isEmpty()) break;
    String lower = line; lower.toLowerCase();
    if (lower.startsWith("content-length:")) contentLength = (size_t)lower.substring(15).toInt();
    if (lower.startsWith("content-type:") && lower.indexOf("application/ipp") >= 0) ipp = true;
  }
  if (!ipp || contentLength < 8 || contentLength > MAX_IPP_BODY) return false;

  uint8_t *buffer = (uint8_t *)ps_malloc(contentLength);
  if (!buffer) buffer = (uint8_t *)malloc(contentLength);
  if (!buffer) return false;

  size_t got = 0;
  deadline = millis() + 15000;
  while (got < contentLength && millis() < deadline) {
    if (!client.available()) { delay(1); continue; }
    int n = client.read(buffer + got, contentLength - got);
    if (n > 0) got += (size_t)n;
  }
  if (got != contentLength) { free(buffer); return false; }
  *body = buffer;
  length = got;
  return true;
}

bool MobileIppServer::buildResponse(const uint8_t *request, size_t length,
                                     uint8_t *response, size_t capacity,
                                     size_t &responseLength) {
  responseLength = 0;
  if (length < 8 || capacity < 128) return false;

  uint16_t version = get16(request);
  const uint16_t operation = get16(request + 2);
  const uint32_t requestId = ((uint32_t)request[4] << 24) | ((uint32_t)request[5] << 16) |
                             ((uint32_t)request[6] << 8) | request[7];
  if (version < 0x0100 || version > 0x0200) version = 0x0200;

  String documentFormat = "application/octet-stream";
  size_t pos = 8;
  bool sawOperationGroup = false;
  size_t documentOffset = length;

  while (pos < length) {
    uint8_t tag = request[pos++];
    if (tag == 0x03) { documentOffset = pos; break; }
    if (tag == 0x01 || tag == 0x02 || tag == 0x04) {
      if (tag == 0x01) sawOperationGroup = true;
      continue;
    }
    if (pos + 4 > length) return false;
    uint16_t nameLen = get16(request + pos); pos += 2;
    if (pos + nameLen + 2 > length) return false;
    String name;
    for (uint16_t i = 0; i < nameLen; ++i) name += (char)request[pos + i];
    pos += nameLen;
    uint16_t valueLen = get16(request + pos); pos += 2;
    if (pos + valueLen > length) return false;
    if (name == "document-format" && valueLen > 0 && valueLen < 128) {
      documentFormat = "";
      for (uint16_t i = 0; i < valueLen; ++i) documentFormat += (char)request[pos + i];
    }
    pos += valueLen;
  }
  if (!sawOperationGroup) return false;

  uint16_t status = 0x0000;
  uint32_t jobId = 0;
  String error;

  if (operation == 0x0002) {
    if (documentOffset >= length) { status = 0x0402; error = "Print-Job has no document"; }
    else if (!handler_) { status = 0x0502; error = "Print backend unavailable"; }
    else if (!handler_(request + documentOffset, length - documentOffset, documentFormat, jobId, error)) {
      status = 0x040A;
      if (error.isEmpty()) error = "Document rejected";
    }
  } else if (operation == 0x0004 || operation == 0x000B || operation == 0x0009 || operation == 0x0008) {
    // Capability/job queries are accepted on the mobile path.
  } else {
    status = 0x0501;
    error = "IPP operation not supported";
  }

  size_t r = 0;
  response[r++] = version >> 8; response[r++] = version & 0xff;
  put16(response + r, status); r += 2;
  put32(response + r, requestId); r += 4;
  response[r++] = 0x01;
  if (!addString(response, capacity, r, 0x47, "attributes-charset", "utf-8")) return false;
  if (!addString(response, capacity, r, 0x48, "attributes-natural-language", "en")) return false;
  if (!error.isEmpty() && !addString(response, capacity, r, 0x41, "status-message", error)) return false;

  if (operation == 0x000B || operation == 0x0004) {
    response[r++] = 0x04;
    if (!addString(response, capacity, r, 0x42, "printer-name", printerName_)) return false;
    if (!addString(response, capacity, r, 0x42, "printer-make-and-model", "ESP32-S3 HP Print Server")) return false;
    if (!addString(response, capacity, r, 0x41, "printer-info", "Smartphone print server")) return false;
    if (!addString(response, capacity, r, 0x45, "printer-uri-supported", printerUri_)) return false;
    if (!addKeyword(response, capacity, r, "uri-authentication-supported", "none")) return false;
    if (!addKeyword(response, capacity, r, "uri-security-supported", "none")) return false;
    if (!addKeyword(response, capacity, r, "ipp-versions-supported", "1.1")) return false;
    if (!addKeyword(response, capacity, r, "ipp-versions-supported", "2.0")) return false;
    if (!addEnum(response, capacity, r, "operations-supported", 0x0002)) return false;
    if (!addEnum(response, capacity, r, "operations-supported", 0x0004)) return false;
    if (!addEnum(response, capacity, r, "operations-supported", 0x0008)) return false;
    if (!addEnum(response, capacity, r, "operations-supported", 0x0009)) return false;
    if (!addEnum(response, capacity, r, "operations-supported", 0x000B)) return false;
    if (!addString(response, capacity, r, 0x47, "charset-configured", "utf-8")) return false;
    if (!addString(response, capacity, r, 0x47, "charset-supported", "utf-8")) return false;
    if (!addString(response, capacity, r, 0x48, "natural-language-configured", "en")) return false;
    if (!addString(response, capacity, r, 0x48, "generated-natural-language-supported", "en")) return false;
    if (!addKeyword(response, capacity, r, "compression-supported", "none")) return false;
    if (!addKeyword(response, capacity, r, "pdl-override-supported", "not-attempted")) return false;
    if (!addString(response, capacity, r, 0x49, "document-format-default", "application/PCLm")) return false;
    if (!addBool(response, capacity, r, "printer-is-accepting-jobs", true)) return false;
    if (!addEnum(response, capacity, r, "printer-state", 3)) return false;
    if (!addKeyword(response, capacity, r, "printer-state-reasons", "none")) return false;
    if (!addInt(response, capacity, r, 0x21, "printer-up-time", millis() / 1000UL)) return false;
    if (!addInt(response, capacity, r, 0x21, "queued-job-count", 0)) return false;
    if (!addMime(response, capacity, r, "image/pwg-raster")) return false;
    if (!addMime(response, capacity, r, "application/PCLm")) return false;
    if (!addMime(response, capacity, r, "application/pdf")) return false;
    if (!addMime(response, capacity, r, "image/jpeg")) return false;
    if (!addMime(response, capacity, r, "image/urf")) return false;
    if (!addKeyword(response, capacity, r, "media-supported", "iso_a4_210x297mm")) return false;
    if (!addKeyword(response, capacity, r, "media-default", "iso_a4_210x297mm")) return false;
    if (!addKeyword(response, capacity, r, "sides-supported", "one-sided")) return false;
    if (!addKeyword(response, capacity, r, "sides-default", "one-sided")) return false;
    if (!addBool(response, capacity, r, "color-supported", true)) return false;
    if (!addInt(response, capacity, r, 0x21, "copies-default", 1)) return false;
    if (!addRange(response, capacity, r, "copies-supported", 1, 99)) return false;
  }

  if (operation == 0x0002) {
    response[r++] = 0x02;
    if (!addInt(response, capacity, r, 0x21, "job-id", jobId)) return false;
    String jobUri = printerUri_ + "/job-" + String(jobId);
    if (!addString(response, capacity, r, 0x45, "job-uri", jobUri)) return false;
    if (!addString(response, capacity, r, 0x45, "job-printer-uri", printerUri_)) return false;
    if (!addString(response, capacity, r, 0x42, "job-name", "Android mobile print job")) return false;
    if (!addString(response, capacity, r, 0x42, "job-originating-user-name", "android")) return false;
    if (!addEnum(response, capacity, r, "job-state", 3)) return false;
    if (!addKeyword(response, capacity, r, "job-state-reasons", "job-incoming")) return false;
    if (!addInt(response, capacity, r, 0x21, "job-printer-up-time", millis() / 1000UL)) return false;
  }

  if (r + 1 > capacity) return false;
  response[r++] = 0x03;
  responseLength = r;
  return true;
}

void MobileIppServer::handleClient(WiFiClient &client) {
  uint8_t *body = nullptr;
  size_t bodyLength = 0;
  uint8_t *response = (uint8_t *)malloc(RESPONSE_CAPACITY);
  size_t responseLength = 0;
  bool ok = response && readHttpBody(client, &body, bodyLength) && buildResponse(body, bodyLength, response, RESPONSE_CAPACITY, responseLength);
  if (!ok) client.print("HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
  else {
    client.print("HTTP/1.1 200 OK\r\nContent-Type: application/ipp\r\nContent-Length: ");
    client.print(responseLength); client.print("\r\nConnection: close\r\n\r\n"); client.write(response, responseLength);
  }
  if (body) free(body);
  if (response) free(response);
  client.stop();
}

void MobileIppServer::poll() {
  WiFiClient client = server_.available();
  if (client) handleClient(client);
}
