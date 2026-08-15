#include "print_transports.h"

static bool connectClient(WiFiClient &client, const PrintTarget &target, uint16_t fallbackPort, uint32_t timeoutMs) {
  client.setTimeout(timeoutMs / 1000 + 1);
  uint16_t port = target.port ? target.port : fallbackPort;
  if (target.address != IPAddress(0,0,0,0)) return client.connect(target.address, port, timeoutMs);
  if (!target.host.isEmpty()) return client.connect(target.host.c_str(), port, timeoutMs);
  return false;
}

bool Raw9100Transport::send(const PrintTarget &target, const uint8_t *data, size_t length, uint32_t timeoutMs) {
  if (!data || length == 0) return false;
  WiFiClient client;
  if (!connectClient(client, target, 9100, timeoutMs)) return false;

  size_t sent = 0;
  while (sent < length) {
    size_t n = client.write(data + sent, length - sent);
    if (n == 0) { client.stop(); return false; }
    sent += n;
    if (!client.connected() && sent < length) { client.stop(); return false; }
    yield();
  }
  client.flush();
  delay(20);
  client.stop();
  return sent == length;
}

static bool writeAll(WiFiClient &client, const uint8_t *data, size_t length) {
  size_t sent = 0;
  while (sent < length) {
    size_t n = client.write(data + sent, length - sent);
    if (n == 0) return false;
    sent += n;
    yield();
  }
  return true;
}

bool LprTransport::send(const PrintTarget &target, const uint8_t *data, size_t length, const String &queue, uint32_t timeoutMs) {
  if (!data || length == 0 || queue.isEmpty()) return false;
  WiFiClient client;
  if (!connectClient(client, target, 515, timeoutMs)) return false;

  // RFC 1179: receive print job command. We use a unique local job name and
  // transmit the document as a data file. The protocol is intentionally kept
  // binary-safe; document data is never converted to String.
  String host = WiFi.getHostname();
  if (host.isEmpty()) host = "esp32";
  String controlName = "cfA001" + host;
  String dataName = "dfA001" + host;
  String control = "H" + host + "\nJ" + dataName + "\nP" + host + "\n"
                 + "l" + dataName + "\nU" + dataName + "\nN" + dataName + "\n";

  uint32_t controlSize = control.length();
  String command = String((char)0x02) + String(controlSize) + " " + controlName + "\n";
  if (!writeAll(client, (const uint8_t *)command.c_str(), command.length())) { client.stop(); return false; }
  if (client.read() != 0) { client.stop(); return false; }

  String dataCommand = String((char)0x03) + String(length) + " " + dataName + "\n";
  if (!writeAll(client, (const uint8_t *)dataCommand.c_str(), dataCommand.length())) { client.stop(); return false; }
  if (client.read() != 0) { client.stop(); return false; }
  if (!writeAll(client, data, length)) { client.stop(); return false; }
  client.write((uint8_t)0);
  if (client.read() != 0) { client.stop(); return false; }

  client.stop();
  (void)queue; // queue selection is encoded by the server-side RFC1179 path in future revision.
  return true;
}

static void ippPut16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v & 0xff; }
static void ippPut32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static bool ippString(uint8_t *buffer, size_t capacity, size_t &pos, uint8_t tag, const String &value) {
  size_t n = value.length();
  if (n > 65535 || pos + 3 + n > capacity) return false;
  buffer[pos++] = tag;
  ippPut16(buffer + pos, (uint16_t)n); pos += 2;
  memcpy(buffer + pos, value.c_str(), n); pos += n;
  return true;
}

bool IppTransport::buildPrintJobRequest(uint8_t *buffer, size_t capacity, size_t &length,
                                        const String &printerUri, const String &user,
                                        uint32_t jobId, const uint8_t *document, size_t documentLength) {
  if (!buffer || !document || printerUri.isEmpty()) return false;
  size_t pos = 0;
  if (capacity < 16) return false;

  // IPP version 2.0, Print-Job operation (0x0002).
  buffer[pos++] = 2; buffer[pos++] = 0;
  ippPut16(buffer + pos, 0x0002); pos += 2;
  ippPut32(buffer + pos, jobId); pos += 4;
  buffer[pos++] = 0x01; // operation-attributes-tag

  if (!ippString(buffer, capacity, pos, 0x42, "uri")) return false;
  if (!ippString(buffer, capacity, pos, 0x45, printerUri)) return false;
  if (!ippString(buffer, capacity, pos, 0x42, "requesting-user-name")) return false;
  if (!ippString(buffer, capacity, pos, 0x41, user.isEmpty() ? "android" : user)) return false;
  if (!ippString(buffer, capacity, pos, 0x42, "document-format")) return false;
  if (!ippString(buffer, capacity, pos, 0x49, "application/octet-stream")) return false;

  if (pos + documentLength > capacity) return false;
  memcpy(buffer + pos, document, documentLength); pos += documentLength;
  length = pos;
  return true;
}
