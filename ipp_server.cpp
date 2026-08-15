#include "ipp_server.h"

static void put16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v & 0xff; }
static void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static uint16_t get16(const uint8_t *p) { return ((uint16_t)p[0] << 8) | p[1]; }
static uint16_t get16le(const uint8_t *p) { return ((uint16_t)p[1] << 8) | p[0]; }

IppServer::IppServer(uint16_t port) : server_(port), port_(port) {}

void IppServer::begin(const String &printerName, PrintJobHandler handler) {
  printerName_ = printerName;
  handler_ = handler;
  server_.begin();
}

bool IppServer::running() const { return true; }

size_t IppServer::addStringAttr(uint8_t *out, size_t capacity, size_t pos, uint8_t tag, const char *name, const String &value) {
  size_t n = value.length(), nameLen = strlen(name);
  if (nameLen > 65535 || n > 65535 || pos + 5 + nameLen + n > capacity) return 0;
  out[pos++] = tag; put16(out + pos, nameLen); pos += 2; memcpy(out + pos, name, nameLen); pos += nameLen;
  put16(out + pos, n); pos += 2; memcpy(out + pos, value.c_str(), n); pos += n;
  return pos;
}

size_t IppServer::addIntegerAttr(uint8_t *out, size_t capacity, size_t pos, uint8_t tag, const char *name, uint32_t value) {
  size_t nameLen = strlen(name);
  if (nameLen > 65535 || pos + 9 + nameLen > capacity) return 0;
  out[pos++] = tag; put16(out + pos, nameLen); pos += 2; memcpy(out + pos, name, nameLen); pos += nameLen;
  put16(out + pos, 4); pos += 2; put32(out + pos, value); pos += 4;
  return pos;
}

size_t IppServer::addEnumAttr(uint8_t *out, size_t capacity, size_t pos, uint8_t tag, const char *name, uint32_t value) {
  return addIntegerAttr(out, capacity, pos, tag, name, value);
}

bool IppServer::readHttp(WiFiClient &client, uint8_t *body, size_t capacity, size_t &bodyLength) {
  bodyLength = 0;
  String requestLine = client.readStringUntil('\n');
  requestLine.trim();
  if (requestLine.length() < 8 || !requestLine.startsWith("POST ")) return false;

  size_t contentLength = 0;
  bool ippContent = false;
  unsigned long deadline = millis() + 3000;
  while (millis() < deadline) {
    if (!client.available()) { delay(1); continue; }
    String line = client.readStringUntil('\n');
    line.trim();
    if (line.isEmpty()) break;
    String lower = line; lower.toLowerCase();
    if (lower.startsWith("content-length:")) contentLength = (size_t)line.substring(15).toInt();
    if (lower.startsWith("content-type:") && lower.indexOf("application/ipp") >= 0) ippContent = true;
  }
  if (!ippContent || contentLength == 0 || contentLength > capacity) return false;

  size_t got = 0;
  deadline = millis() + 5000;
  while (got < contentLength && millis() < deadline) {
    if (!client.available()) { delay(1); continue; }
    int n = client.read(body + got, contentLength - got);
    if (n > 0) got += n;
  }
  bodyLength = got;
  return got == contentLength;
}

bool IppServer::handleIpp(const uint8_t *request, size_t length, uint8_t *response, size_t capacity, size_t &responseLength) {
  responseLength = 0;
  if (length < 8 || capacity < 64) return false;
  uint16_t version = get16(request);
  uint16_t operation = get16(request + 2);
  uint32_t requestId = ((uint32_t)request[4] << 24) | ((uint32_t)request[5] << 16) | ((uint32_t)request[6] << 8) | request[7];
  if (version == 0 || version > 0x0200) version = 0x0200;

  // Find the document boundary and document-format from the operation attributes.
  size_t pos = 8;
  if (request[pos++] != 0x01) return false; // operation-attributes-tag
  String documentFormat = "application/octet-stream";
  while (pos + 3 <= length) {
    uint8_t tag = request[pos++];
    if (tag == 0x03 || tag == 0x04) break; // end-of-attributes / job attrs
    if (pos + 2 > length) return false;
    uint16_t nameLen = get16(request + pos); pos += 2;
    if (pos + nameLen + 2 > length) return false;
    String name;
    for (uint16_t i = 0; i < nameLen; ++i) name += (char)request[pos + i];
    pos += nameLen;
    uint16_t valueLen = get16(request + pos); pos += 2;
    if (pos + valueLen > length) return false;
    if (name == "document-format" && valueLen < 128) {
      documentFormat = "";
      for (uint16_t i = 0; i < valueLen; ++i) documentFormat += (char)request[pos + i];
    }
    pos += valueLen;
  }
  size_t documentOffset = pos;

  size_t r = 0;
  response[r++] = version >> 8; response[r++] = version & 0xff;
  uint16_t status = 0x0000;
  if (operation == 0x000B) status = 0x0400; // Get-Printer-Attributes not supported by old clients? overridden below
  if (operation == 0x0002) status = 0x0000; // Print-Job
  put16(response + r, status); r += 2;
  put32(response + r, requestId); r += 4;
  response[r++] = 0x01; // operation attributes

  r = addStringAttr(response, capacity, r, 0x42, "attributes-charset", "utf-8");
  r = addStringAttr(response, capacity, r, 0x48, "attributes-natural-language", "en");
  if (!r) return false;

  if (operation == 0x000B) {
    response[r++] = 0x04; // printer-attributes-tag
    r = addStringAttr(response, capacity, r, 0x44, "printer-name", printerName_);
    r = addStringAttr(response, capacity, r, 0x45, "printer-uri-supported", "ipp://hp-print-server.local/ipp/print");
    r = addStringAttr(response, capacity, r, 0x49, "document-format-supported", "application/pdf");
    r = addStringAttr(response, capacity, r, 0x49, "document-format-supported", "image/pwg-raster");
    r = addStringAttr(response, capacity, r, 0x49, "document-format-supported", "image/urf");
    r = addEnumAttr(response, capacity, r, 0x23, "printer-state", 3);
    r = addIntegerAttr(response, capacity, r, 0x21, "queued-job-count", 0);
  } else if (operation == 0x0002) {
    if (documentOffset > length) return false;
    String error;
    bool accepted = handler_ ? handler_(request + documentOffset, length - documentOffset, documentFormat, error) : false;
    if (!accepted) {
      status = 0x0404; // server-error-operation-not-supported until a backend accepts it
      response[2] = status >> 8; response[3] = status & 0xff;
      r = addStringAttr(response, capacity, r, 0x41, "status-message", error.isEmpty() ? "Print backend unavailable" : error);
    }
    response[r++] = 0x02; // job attributes
    r = addIntegerAttr(response, capacity, r, 0x23, "job-state", accepted ? 3 : 8);
  } else {
    status = 0x0501; // client-error-bad-request
    response[2] = status >> 8; response[3] = status & 0xff;
    r = addStringAttr(response, capacity, r, 0x41, "status-message", "Unsupported IPP operation");
  }
  if (!r || r + 1 > capacity) return false;
  response[r++] = 0x03;
  responseLength = r;
  return true;
}

void IppServer::handleClient(WiFiClient &client) {
  static uint8_t request[32768];
  static uint8_t response[8192];
  size_t requestLength = 0, responseLength = 0;
  client.setTimeout(5);
  bool ok = readHttp(client, request, sizeof(request), requestLength) && handleIpp(request, requestLength, response, sizeof(response), responseLength);
  if (!ok) {
    client.print("HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
    client.stop();
    return;
  }
  client.print("HTTP/1.1 200 OK\r\nContent-Type: application/ipp\r\nContent-Length: ");
  client.print(responseLength);
  client.print("\r\nConnection: close\r\n\r\n");
  client.write(response, responseLength);
  client.stop();
}

void IppServer::poll() {
  WiFiClient client = server_.available();
  if (client) handleClient(client);
}
