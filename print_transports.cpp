#include "print_transports.h"
#include <WiFi.h>
#include <string.h>

static bool connectClient(WiFiClient& c, const PrintTarget& t, uint16_t fallback,
                          uint32_t timeout) {
  c.setTimeout(timeout / 1000 + 1);
  uint16_t port = t.port ? t.port : fallback;
  if (t.address != IPAddress(0, 0, 0, 0)) {
    return c.connect(t.address, port, timeout);
  }
  return !t.host.isEmpty() && c.connect(t.host.c_str(), port, timeout);
}

static bool writeAll(WiFiClient& c, const uint8_t* d, size_t n) {
  size_t sent = 0;
  while (sent < n) {
    size_t w = c.write(d + sent, n - sent);
    if (!w) return false;
    sent += w;
    yield();
  }
  return true;
}

static bool writeStream(WiFiClient& c, Stream& source, size_t n) {
  uint8_t buffer[1460];
  size_t remaining = n;
  while (remaining) {
    size_t want = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
    size_t got = source.readBytes((char*)buffer, want);
    if (got != want) return false;
    if (!writeAll(c, buffer, got)) return false;
    remaining -= got;
    yield();
  }
  return true;
}

bool Raw9100Transport::send(const PrintTarget& t, const uint8_t* d, size_t n,
                            uint32_t timeout) {
  if (!d || !n) return false;

  WiFiClient c;
  if (!connectClient(c, t, 9100, timeout)) return false;

  bool ok = writeAll(c, d, n);
  c.flush();
  delay(20);
  c.stop();
  return ok;
}

bool Raw9100Transport::sendStream(const PrintTarget& t, Stream& source,
                                  size_t n, uint32_t timeout) {
  if (!n) return false;

  WiFiClient c;
  if (!connectClient(c, t, 9100, timeout)) return false;

  bool ok = writeStream(c, source, n);
  c.flush();
  delay(20);
  c.stop();
  return ok;
}

static void put16(uint8_t* p, uint16_t v) {
  p[0] = v >> 8;
  p[1] = v;
}

static void put32(uint8_t* p, uint32_t v) {
  p[0] = v >> 24;
  p[1] = v >> 16;
  p[2] = v >> 8;
  p[3] = v;
}

static bool attr(uint8_t* b, size_t cap, size_t& pos, uint8_t tag,
                 const char* name, const uint8_t* v, size_t n) {
  size_t nl = strlen(name);
  if (nl > 65535 || n > 65535 || pos + 5 + nl + n > cap) return false;

  b[pos++] = tag;
  put16(b + pos, nl);
  pos += 2;
  memcpy(b + pos, name, nl);
  pos += nl;
  put16(b + pos, n);
  pos += 2;
  memcpy(b + pos, v, n);
  pos += n;
  return true;
}

static bool sattr(uint8_t* b, size_t cap, size_t& pos, uint8_t tag,
                  const char* n, const String& v) {
  return attr(b, cap, pos, tag, n, (const uint8_t*)v.c_str(), v.length());
}

static bool iattr(uint8_t* b, size_t cap, size_t& pos, uint8_t tag,
                  const char* n, uint32_t v) {
  uint8_t x[4];
  put32(x, v);
  return attr(b, cap, pos, tag, n, x, 4);
}

bool IppTransport::buildPrintJobRequest(uint8_t* b, size_t cap, size_t& length,
                                         const String& uri, const String& user,
                                         uint32_t requestId, const uint8_t* d,
                                         size_t n) {
  length = 0;
  if (!b || !d || !uri.length() || n > cap) return false;
  size_t p = 0;
  if (cap < 64) return false;

  b[p++] = 1;
  b[p++] = 1;
  put16(b + p, 0x0002);
  p += 2;
  put32(b + p, requestId);
  p += 4;
  b[p++] = 1;

  if (!sattr(b, cap, p, 0x47, "attributes-charset", "utf-8") ||
      !sattr(b, cap, p, 0x48, "attributes-natural-language", "en") ||
      !sattr(b, cap, p, 0x45, "printer-uri", uri) ||
      !sattr(b, cap, p, 0x42, "requesting-user-name",
             user.isEmpty() ? String("android") : user) ||
      !sattr(b, cap, p, 0x49, "document-format", "application/octet-stream")) {
    return false;
  }

  if (p + 1 + n > cap) return false;
  b[p++] = 3;
  memcpy(b + p, d, n);
  p += n;
  length = p;
  return true;
}

static bool buildIppHeader(uint8_t* b, size_t cap, size_t& length,
                           const String& uri, const String& user,
                           uint32_t requestId, const String& documentFormat,
                           size_t documentLength) {
  length = 0;
  if (!b || !uri.length()) return false;
  size_t p = 0;
  if (cap < 128) return false;

  b[p++] = 1;
  b[p++] = 1;
  put16(b + p, 0x0002);
  p += 2;
  put32(b + p, requestId);
  p += 4;
  b[p++] = 1;

  if (!sattr(b, cap, p, 0x47, "attributes-charset", "utf-8") ||
      !sattr(b, cap, p, 0x48, "attributes-natural-language", "en") ||
      !sattr(b, cap, p, 0x45, "printer-uri", uri) ||
      !sattr(b, cap, p, 0x42, "requesting-user-name",
             user.isEmpty() ? String("esp32") : user) ||
      !sattr(b, cap, p, 0x49, "document-format",
             documentFormat.isEmpty() ? String("application/octet-stream") : documentFormat)) {
    return false;
  }

  // End of attributes; the caller streams the document immediately after it.
  b[p++] = 3;
  length = p;
  (void)documentLength;
  return true;
}

static bool readHttpStatus(WiFiClient& c, uint32_t timeoutMs) {
  unsigned long deadline = millis() + timeoutMs;
  String line;
  while ((long)(deadline - millis()) > 0) {
    if (!c.available()) { delay(1); continue; }
    line = c.readStringUntil('\n');
    line.trim();
    return line.startsWith("HTTP/1.1 200") || line.startsWith("HTTP/1.0 200");
  }
  return false;
}

static bool readIppResponseStatus(WiFiClient& c, uint16_t* status,
                                  uint32_t timeoutMs) {
  if (status) *status = 0xFFFF;
  uint8_t h[8];
  size_t got = 0;
  unsigned long deadline = millis() + timeoutMs;
  while (got < sizeof(h) && (long)(deadline - millis()) > 0) {
    if (!c.available()) { delay(1); continue; }
    int n = c.read(h + got, sizeof(h) - got);
    if (n > 0) got += (size_t)n;
  }
  if (got != sizeof(h)) return false;
  if (status) *status = ((uint16_t)h[2] << 8) | h[3];
  return h[0] == 1 && (h[1] == 0 || h[1] == 1 || h[1] == 2);
}

static bool sendIppStream(const PrintTarget& target, const String& printerUri,
                          const String& user, Stream& source, size_t documentLength,
                          uint32_t timeoutMs, uint16_t* ippStatus) {
  if (!documentLength || printerUri.isEmpty()) return false;

  WiFiClient c;
  if (!connectClient(c, target, 631, timeoutMs)) return false;
  c.setTimeout(timeoutMs / 1000 + 1);

  uint8_t header[1024];
  size_t headerLength = 0;
  const uint32_t requestId = millis() | 1UL;
  if (!buildIppHeader(header, sizeof(header), headerLength, printerUri, user,
                      requestId, "application/octet-stream", documentLength)) {
    c.stop();
    return false;
  }

  c.print("POST ");
  int scheme = printerUri.indexOf("://");
  int slash = scheme >= 0 ? printerUri.indexOf('/', scheme + 3) : -1;
  String path = slash >= 0 ? printerUri.substring(slash) : String("/ipp/print");
  if (path.isEmpty()) path = "/ipp/print";
  c.print(path);
  c.print(" HTTP/1.1\r\nHost: ");
  if (target.host.length()) c.print(target.host);
  else c.print(target.address.toString());
  c.print("\r\nContent-Type: application/ipp\r\nContent-Length: ");
  c.print((unsigned long)(headerLength + documentLength));
  c.print("\r\nConnection: close\r\n\r\n");

  if (!writeAll(c, header, headerLength) || !writeStream(c, source, documentLength)) {
    c.stop();
    return false;
  }
  c.flush();

  const bool httpOk = readHttpStatus(c, timeoutMs);
  if (!httpOk) { c.stop(); return false; }
  const bool ippOk = readIppResponseStatus(c, ippStatus, timeoutMs);
  c.stop();
  return ippOk && (!ippStatus || *ippStatus == 0x0000);
}

bool IppTransport::sendStream(const PrintTarget& target, const String& printerUri,
                              const String& user, Stream& source, size_t documentLength,
                              uint32_t timeoutMs, uint16_t* ippStatus) {
  return sendIppStream(target, printerUri, user, source, documentLength,
                       timeoutMs, ippStatus);
}

bool IppTransport::send(const PrintTarget& target, const String& printerUri,
                        const String& user, const uint8_t* document,
                        size_t documentLength, uint32_t timeoutMs,
                        uint16_t* ippStatus) {
  if (!document || !documentLength) return false;
  class MemoryStream : public Stream {
  public:
    MemoryStream(const uint8_t* d, size_t n) : d_(d), n_(n) {}
    int available() override { return (int)(n_ - p_); }
    int read() override { return p_ < n_ ? d_[p_++] : -1; }
    int peek() override { return p_ < n_ ? d_[p_] : -1; }
    size_t readBytes(char* b, size_t n) override {
      size_t k = n < n_ - p_ ? n : n_ - p_;
      if (k) memcpy(b, d_ + p_, k);
      p_ += k;
      return k;
    }
    void flush() override {}
    size_t write(uint8_t) override { return 0; }
    using Stream::readBytes;
  private:
    const uint8_t* d_; size_t n_; size_t p_ = 0;
  } source(document, documentLength);
  return sendStream(target, printerUri, user, source, documentLength,
                    timeoutMs, ippStatus);
}
