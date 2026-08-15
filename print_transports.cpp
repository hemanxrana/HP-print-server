#include "print_transports.h"

static bool connectClient(WiFiClient &client, const PrintTarget &target, uint16_t fallbackPort, uint32_t timeoutMs) {
  client.setTimeout(timeoutMs / 1000 + 1);
  uint16_t port = target.port ? target.port : fallbackPort;
  if (target.address != IPAddress(0,0,0,0)) return client.connect(target.address, port, timeoutMs);
  if (!target.host.isEmpty()) return client.connect(target.host.c_str(), port, timeoutMs);
  return false;
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

static bool readLprAck(WiFiClient &client) {
  int value = client.read();
  return value == 0;
}

bool Raw9100Transport::send(const PrintTarget &target, const uint8_t *data, size_t length, uint32_t timeoutMs) {
  if (!data || length == 0) return false;
  WiFiClient client;
  if (!connectClient(client, target, 9100, timeoutMs)) return false;
  bool ok = writeAll(client, data, length);
  client.flush();
  delay(20);
  client.stop();
  return ok;
}

bool LprTransport::send(const PrintTarget &target, const uint8_t *data, size_t length, const String &queue, uint32_t timeoutMs) {
  if (!data || length == 0 || queue.isEmpty() || queue.length() > 255) return false;
  WiFiClient client;
  if (!connectClient(client, target, 515, timeoutMs)) return false;

  // RFC 1179: select the remote queue first.
  uint8_t queueCommand = 0x02;
  if (!client.write(&queueCommand, 1) || !writeAll(client, (const uint8_t *)queue.c_str(), queue.length()) || !client.write((uint8_t)'\n')) {
    client.stop(); return false;
  }
  if (!readLprAck(client)) { client.stop(); return false; }

  String host = WiFi.getHostname();
  if (host.isEmpty()) host = "esp32";
  // Keep filenames short enough for traditional LPD implementations.
  String controlName = "cfA001esp32";
  String dataName = "dfA001esp32";
  String control = "H" + host + "\nJ" + dataName + "\nP" + host + "\nl" + dataName + "\nU" + dataName + "\nN" + dataName + "\n";

  // Receive control file.
  uint8_t controlCommand = 0x02;
  if (!client.write(&controlCommand, 1)) { client.stop(); return false; }
  String controlHeader = String(control.length()) + " " + controlName + "\n";
  if (!writeAll(client, (const uint8_t *)controlHeader.c_str(), controlHeader.length()) || !readLprAck(client)) { client.stop(); return false; }
  if (!writeAll(client, (const uint8_t *)control.c_str(), control.length()) || !client.write((uint8_t)0) || !readLprAck(client)) { client.stop(); return false; }

  // Receive data file.
  uint8_t dataCommand = 0x03;
  if (!client.write(&dataCommand, 1)) { client.stop(); return false; }
  String dataHeader = String(length) + " " + dataName + "\n";
  if (!writeAll(client, (const uint8_t *)dataHeader.c_str(), dataHeader.length()) || !readLprAck(client)) { client.stop(); return false; }
  if (!writeAll(client, data, length) || !client.write((uint8_t)0) || !readLprAck(client)) { client.stop(); return false; }

  client.stop();
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
  if (capacity < 16 || documentLength > capacity - 8) return false;

  buffer[pos++] = 2; buffer[pos++] = 0;
  ippPut16(buffer + pos, 0x0002); pos += 2;
  ippPut32(buffer + pos, jobId); pos += 4;
  buffer[pos++] = 0x01;

  if (!ippString(buffer, capacity, pos, 0x42, "printer-uri")) return false;
  if (!ippString(buffer, capacity, pos, 0x45, printerUri)) return false;
  if (!ippString(buffer, capacity, pos, 0x42, "requesting-user-name")) return false;
  if (!ippString(buffer, capacity, pos, 0x41, user.isEmpty() ? "android" : user)) return false;
  if (!ippString(buffer, capacity, pos, 0x42, "document-format")) return false;
  if (!ippString(buffer, capacity, pos, 0x49, "application/octet-stream")) return false;

  if (pos + documentLength > capacity) return false;
  memcpy(buffer + pos, document, documentLength);
  pos += documentLength;
  length = pos;
  return true;
}
