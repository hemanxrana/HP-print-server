#include "usb_scanner_backend.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/usb_host.h"
#include "usb/usb_helpers.h"
#include "usb/usb_types_ch9.h"
#include <cstring>
#include <cstdlib>

namespace {

static constexpr uint8_t USB_PRINTER_CLASS = 0x07;
static constexpr uint8_t USB_PRINTER_SUBCLASS = 0x01;
static constexpr uint8_t USB_IPP_OVER_USB_PROTOCOL = 0x04;
static constexpr uint8_t USB_XFER_BULK = 0x02;
static constexpr int MAX_DEVICE_ADDRESSES = 8;
static constexpr int MAX_SCANNER_CANDIDATES = 8;

static constexpr size_t HTTP_HEADER_LIMIT = 16384;
static constexpr size_t HTTP_REQUEST_BODY_LIMIT = 65536;
static constexpr size_t USB_IO_CHUNK = 4096;
static constexpr uint32_t NETWORK_HEADER_TIMEOUT_MS = 15000;
static constexpr uint32_t NETWORK_BODY_TIMEOUT_MS = 30000;
static constexpr uint32_t USB_RESPONSE_TIMEOUT_MS = 180000;

struct Endpoint {
  uint8_t address = 0;
  uint16_t maxPacket = 0;
  bool valid() const { return address != 0 && maxPacket != 0; }
};

struct ScannerInterface {
  bool found = false;
  uint8_t interfaceNumber = 0;
  uint8_t alternateSetting = 0;
  Endpoint bulkOut;
  Endpoint bulkIn;

  bool usable() const {
    return found && bulkOut.valid() && bulkIn.valid() &&
           (bulkOut.address & 0x80) == 0 &&
           (bulkIn.address & 0x80) != 0;
  }
};

struct TransferWait {
  SemaphoreHandle_t done = nullptr;
};

struct Runtime {
  usb_host_client_handle_t client = nullptr;
  usb_device_handle_t device = nullptr;

  volatile bool newDevice = false;
  volatile uint8_t newAddress = 0;
  volatile bool deviceGone = false;
  volatile bool ioActive = false;

  bool deviceOpen = false;
  bool interfaceClaimed = false;
  ScannerInterface iface;

  volatile bool ready = false;
  volatile bool busy = false;

  TaskHandle_t usbTask = nullptr;
  TaskHandle_t proxyTask = nullptr;

  WiFiServer *server = nullptr;
  bool serverStarted = false;
  bool mdnsAdvertised = false;
} g;

static bool isBulk(const usb_ep_desc_t *ep) {
  return ep && ((ep->bmAttributes & 0x03) == USB_XFER_BULK);
}

static void transferCallback(usb_transfer_t *t) {
  if (!t) return;
  TransferWait *wait = static_cast<TransferWait *>(t->context);
  if (wait && wait->done) xSemaphoreGive(wait->done);
}

static void releaseScannerDevice() {
  g.ready = false;
  if (!g.deviceOpen || !g.device) {
    g.device = nullptr;
    g.deviceOpen = false;
    g.interfaceClaimed = false;
    g.iface = ScannerInterface{};
    return;
  }

  if (g.interfaceClaimed) {
    usb_host_interface_release(g.client, g.device, g.iface.interfaceNumber);
    g.interfaceClaimed = false;
  }
  usb_host_device_close(g.client, g.device);
  g.device = nullptr;
  g.deviceOpen = false;
  g.iface = ScannerInterface{};
  Serial.println("[SCAN] IPP-over-USB scanner interface released");
}

static int scannerScore(const ScannerInterface &s) {
  if (!s.usable()) return -10000;
  int score = 1000;
  score -= (int)s.interfaceNumber * 20;
  if (s.alternateSetting == 1) score += 5;
  return score;
}

static bool openScannerDevice(uint8_t address) {
  usb_device_handle_t dev = nullptr;
  esp_err_t err = usb_host_device_open(g.client, address, &dev);
  if (err != ESP_OK || !dev) return false;

  const usb_config_desc_t *config = nullptr;
  err = usb_host_get_active_config_descriptor(dev, &config);
  if (err != ESP_OK || !config) {
    usb_host_device_close(g.client, dev);
    return false;
  }

  ScannerInterface candidates[MAX_SCANNER_CANDIDATES];
  uint8_t candidateCount = 0;
  ScannerInterface *current = nullptr;

  int offset = 0;
  const uint16_t total = config->wTotalLength;
  const usb_standard_desc_t *desc =
      usb_parse_next_descriptor((const usb_standard_desc_t *)config, total, &offset);

  while (desc) {
    if (desc->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
      const usb_intf_desc_t *intf = (const usb_intf_desc_t *)desc;
      current = nullptr;

      if (intf->bInterfaceClass == USB_PRINTER_CLASS &&
          intf->bInterfaceSubClass == USB_PRINTER_SUBCLASS &&
          intf->bInterfaceProtocol == USB_IPP_OVER_USB_PROTOCOL &&
          candidateCount < MAX_SCANNER_CANDIDATES) {
        current = &candidates[candidateCount++];
        current->found = true;
        current->interfaceNumber = intf->bInterfaceNumber;
        current->alternateSetting = intf->bAlternateSetting;
      }
    } else if (desc->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT) {
      const usb_ep_desc_t *ep = (const usb_ep_desc_t *)desc;
      if (current && isBulk(ep)) {
        Endpoint slot;
        slot.address = ep->bEndpointAddress;
        slot.maxPacket = ep->wMaxPacketSize;
        if ((ep->bEndpointAddress & 0x80) != 0) current->bulkIn = slot;
        else current->bulkOut = slot;
      }
    }
    desc = usb_parse_next_descriptor(desc, total, &offset);
  }

  int best = -1;
  int bestScore = -10001;
  for (uint8_t i = 0; i < candidateCount; ++i) {
    const int score = scannerScore(candidates[i]);
    Serial.printf("[SCAN] IPP-over-USB candidate IF=%u ALT=%u OUT=0x%02X IN=0x%02X score=%d\n",
                  candidates[i].interfaceNumber,
                  candidates[i].alternateSetting,
                  candidates[i].bulkOut.address,
                  candidates[i].bulkIn.address,
                  score);
    if (score > bestScore) {
      bestScore = score;
      best = i;
    }
  }

  if (best < 0) {
    usb_host_device_close(g.client, dev);
    return false;
  }

  ScannerInterface chosen = candidates[best];
  err = usb_host_interface_claim(g.client, dev,
                                 chosen.interfaceNumber,
                                 chosen.alternateSetting);
  if (err != ESP_OK) {
    Serial.printf("[SCAN] Could not claim IF=%u ALT=%u: %s\n",
                  chosen.interfaceNumber, chosen.alternateSetting,
                  esp_err_to_name(err));
    usb_host_device_close(g.client, dev);
    return false;
  }

  g.device = dev;
  g.deviceOpen = true;
  g.interfaceClaimed = true;
  g.iface = chosen;
  g.ready = true;

  Serial.printf("[SCAN] eSCL transport ready on USB IF=%u ALT=%u OUT=0x%02X IN=0x%02X\n",
                chosen.interfaceNumber, chosen.alternateSetting,
                chosen.bulkOut.address, chosen.bulkIn.address);
  return true;
}

static void scanExistingDevices() {
  if (g.deviceOpen) return;

  uint8_t addresses[MAX_DEVICE_ADDRESSES] = {};
  int count = 0;
  if (usb_host_device_addr_list_fill(MAX_DEVICE_ADDRESSES, addresses, &count) != ESP_OK) return;

  for (int i = 0; i < count && !g.deviceOpen; ++i) {
    openScannerDevice(addresses[i]);
  }
}

static void scannerClientEvent(const usb_host_client_event_msg_t *event, void *) {
  if (!event) return;

  if (event->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
    if (!g.deviceOpen && !g.newDevice) {
      g.newAddress = event->new_dev.address;
      g.newDevice = true;
    }
  } else if (event->event == USB_HOST_CLIENT_EVENT_DEV_GONE &&
             g.deviceOpen && event->dev_gone.dev_hdl == g.device) {
    g.deviceGone = true;
    g.ready = false;
  }
}

static void scannerUsbTask(void *) {
  usb_host_client_config_t config{};
  config.is_synchronous = false;
  config.max_num_event_msg = 8;
  config.async.client_event_callback = scannerClientEvent;
  config.async.callback_arg = nullptr;

  const esp_err_t registered = usb_host_client_register(&config, &g.client);
  if (registered != ESP_OK) {
    Serial.printf("[SCAN] USB client registration failed: %s; printing is unaffected\n",
                  esp_err_to_name(registered));
    vTaskDelete(nullptr);
    return;
  }

  Serial.println("[SCAN] Independent USB scanner client registered");
  scanExistingDevices();

  while (true) {
    const esp_err_t eventResult =
        usb_host_client_handle_events(g.client, pdMS_TO_TICKS(50));
    if (eventResult != ESP_OK && eventResult != ESP_ERR_TIMEOUT) {
      Serial.printf("[SCAN] USB event error: %s\n", esp_err_to_name(eventResult));
    }

    if (g.deviceGone && !g.ioActive) {
      g.deviceGone = false;
      releaseScannerDevice();
    }

    if (g.newDevice && !g.deviceOpen) {
      const uint8_t address = g.newAddress;
      g.newDevice = false;
      openScannerDevice(address);
    }
  }
}

static bool scannerTransfer(uint8_t endpoint,
                            uint8_t *data,
                            size_t length,
                            bool input,
                            size_t &actual,
                            uint32_t timeoutMs,
                            String &error) {
  actual = 0;
  if (!data || !length) {
    error = "empty scanner USB transfer";
    return false;
  }
  if (!g.ready || !g.deviceOpen || !g.interfaceClaimed || !g.device) {
    error = "scanner USB interface is not ready";
    return false;
  }

  usb_transfer_t *transfer = nullptr;
  esp_err_t err = usb_host_transfer_alloc(length, 0, &transfer);
  if (err != ESP_OK || !transfer) {
    error = String("scanner transfer allocation failed: ") + esp_err_to_name(err);
    return false;
  }

  TransferWait wait;
  wait.done = xSemaphoreCreateBinary();
  if (!wait.done) {
    usb_host_transfer_free(transfer);
    error = "scanner transfer semaphore allocation failed";
    return false;
  }

  if (!input) memcpy(transfer->data_buffer, data, length);
  transfer->num_bytes = length;
  transfer->device_handle = g.device;
  transfer->bEndpointAddress = endpoint;
  transfer->callback = transferCallback;
  transfer->context = &wait;
  transfer->timeout_ms = timeoutMs;

  g.ioActive = true;
  err = usb_host_transfer_submit(transfer);
  if (err != ESP_OK) {
    g.ioActive = false;
    vSemaphoreDelete(wait.done);
    usb_host_transfer_free(transfer);
    error = String("scanner transfer submit failed: ") + esp_err_to_name(err);
    return false;
  }

  const TickType_t ticks =
      timeoutMs ? pdMS_TO_TICKS(timeoutMs + 1000) : portMAX_DELAY;
  if (xSemaphoreTake(wait.done, ticks) != pdTRUE) {
    usb_host_endpoint_halt(transfer->device_handle, endpoint);
    usb_host_endpoint_flush(transfer->device_handle, endpoint);
    usb_host_endpoint_clear(transfer->device_handle, endpoint);
    xSemaphoreTake(wait.done, portMAX_DELAY);
  }

  actual = (size_t)transfer->actual_num_bytes;
  const usb_transfer_status_t status = transfer->status;
  if (input && actual > 0) memcpy(data, transfer->data_buffer, actual);

  vSemaphoreDelete(wait.done);
  usb_host_transfer_free(transfer);
  g.ioActive = false;

  if (status != USB_TRANSFER_STATUS_COMPLETED) {
    error = String("scanner USB transfer status=") + String((int)status);
    return false;
  }
  if (!input && actual != length) {
    error = String("scanner USB short write ") + String((unsigned)actual) +
            "/" + String((unsigned)length);
    return false;
  }
  return true;
}

static bool scannerWriteAll(const uint8_t *data, size_t length, String &error) {
  size_t offset = 0;
  while (offset < length) {
    const size_t part = min((size_t)1024, length - offset);
    size_t actual = 0;
    if (!scannerTransfer(g.iface.bulkOut.address,
                         const_cast<uint8_t *>(data + offset),
                         part, false, actual, 30000, error)) {
      return false;
    }
    offset += actual;
  }
  return true;
}

static bool scannerReadSome(uint8_t *data,
                            size_t capacity,
                            size_t &actual,
                            uint32_t timeoutMs,
                            String &error) {
  return scannerTransfer(g.iface.bulkIn.address,
                         data, capacity, true, actual, timeoutMs, error);
}

static int headerContentLength(const String &header) {
  String lower = header;
  lower.toLowerCase();
  int start = 0;
  while (start < lower.length()) {
    int end = lower.indexOf("\r\n", start);
    if (end < 0) end = lower.length();
    String line = lower.substring(start, end);
    if (line.startsWith("content-length:")) {
      String value = line.substring(strlen("content-length:"));
      value.trim();
      return value.toInt();
    }
    start = end + 2;
  }
  return -1;
}

static bool headerIsChunked(const String &header) {
  String lower = header;
  lower.toLowerCase();
  return lower.indexOf("transfer-encoding: chunked") >= 0;
}

static int responseStatusCode(const String &header) {
  const int firstSpace = header.indexOf(' ');
  if (firstSpace < 0 || firstSpace + 4 > header.length()) return 0;
  return header.substring(firstSpace + 1, firstSpace + 4).toInt();
}

static String requestMethod(const String &header) {
  const int space = header.indexOf(' ');
  return space > 0 ? header.substring(0, space) : String();
}

static bool requestHasExpectContinue(const String &header) {
  String lower = header;
  lower.toLowerCase();
  return lower.indexOf("expect: 100-continue") >= 0;
}

static String rewriteRequestHeader(const String &header) {
  String output;
  output.reserve(header.length() + 64);

  int start = 0;
  bool first = true;
  while (start < header.length()) {
    int end = header.indexOf("\r\n", start);
    if (end < 0) break;
    String line = header.substring(start, end);
    start = end + 2;

    if (first) {
      first = false;
      if (line.endsWith(" HTTP/1.0")) {
        line.remove(line.length() - strlen("HTTP/1.0"));
        line += "HTTP/1.1";
      }
      output += line + "\r\n";
      continue;
    }
    if (line.isEmpty()) break;

    String lower = line;
    lower.toLowerCase();
    if (lower.startsWith("host:") ||
        lower.startsWith("connection:") ||
        lower.startsWith("proxy-connection:") ||
        lower.startsWith("expect:") ||
        lower.startsWith("upgrade:")) {
      continue;
    }
    output += line + "\r\n";
  }

  output += "Host: localhost\r\n";
  output += "Connection: keep-alive\r\n\r\n";
  return output;
}

static String rewriteResponseHeader(const String &header) {
  String output;
  output.reserve(header.length() + 96);

  int start = 0;
  bool first = true;

  while (start < header.length()) {
    int end = header.indexOf("\r\n", start);
    if (end < 0) break;
    String line = header.substring(start, end);
    start = end + 2;

    if (first) {
      first = false;
      output += line + "\r\n";
      continue;
    }
    if (line.isEmpty()) break;

    String lower = line;
    lower.toLowerCase();
    if (lower.startsWith("connection:") ||
        lower.startsWith("proxy-connection:") ||
        lower.startsWith("keep-alive:")) {
      continue;
    }

    if (lower.startsWith("location:")) {
      const int escl = line.indexOf("/eSCL/");
      if (escl >= 0) {
        output += "Location: http://printer.local:";
        output += String(UsbScannerBackend::NETWORK_PORT);
        output += line.substring(escl);
        output += "\r\n";
        continue;
      }
    }

    output += line + "\r\n";
  }

  output += "Connection: close\r\n\r\n";
  return output;
}

static bool writeNetwork(WiFiClient &client,
                         const uint8_t *data,
                         size_t length) {
  if (!data || !length) return true;

  size_t offset = 0;
  unsigned long lastProgress = millis();
  while (offset < length) {
    if (!client.connected()) return false;

    const int writable = client.availableForWrite();
    if (writable > 0) {
      const size_t part = min(length - offset, (size_t)writable);
      const size_t sent = client.write(data + offset, part);
      if (sent > 0) {
        offset += sent;
        lastProgress = millis();
        continue;
      }
    }

    if (millis() - lastProgress > 15000) return false;
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  return true;
}

static void sendLocalHttpError(WiFiClient &client,
                               int code,
                               const char *reason,
                               const char *message) {
  String body = message ? String(message) : String(reason);
  String response = "HTTP/1.1 " + String(code) + " " + String(reason) + "\r\n";
  response += "Content-Type: text/plain; charset=utf-8\r\n";
  response += "Content-Length: " + String(body.length()) + "\r\n";
  response += "Connection: close\r\n\r\n";
  response += body;
  client.write((const uint8_t *)response.c_str(), response.length());
}

static bool readNetworkRequest(WiFiClient &client,
                               String &header,
                               uint8_t *&body,
                               size_t &bodyLength,
                               String &error) {
  header = "";
  body = nullptr;
  bodyLength = 0;
  header.reserve(2048);

  unsigned long lastData = millis();
  while (header.indexOf("\r\n\r\n") < 0) {
    while (client.available() > 0) {
      const int c = client.read();
      if (c < 0) break;
      header += (char)c;
      lastData = millis();
      if (header.length() > HTTP_HEADER_LIMIT) {
        error = "HTTP request header too large";
        return false;
      }
      if (header.endsWith("\r\n\r\n")) break;
    }

    if (header.indexOf("\r\n\r\n") >= 0) break;
    if (!client.connected()) {
      error = "HTTP client disconnected before request header completed";
      return false;
    }
    if (millis() - lastData > NETWORK_HEADER_TIMEOUT_MS) {
      error = "HTTP request header timeout";
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }

  const int contentLength = headerContentLength(header);
  if (contentLength < 0) {
    bodyLength = 0;
    return true;
  }
  if ((size_t)contentLength > HTTP_REQUEST_BODY_LIMIT) {
    error = "HTTP request body exceeds 64 KiB proxy limit";
    return false;
  }

  bodyLength = (size_t)contentLength;
  if (!bodyLength) return true;

  if (requestHasExpectContinue(header)) {
    static const char continueResponse[] = "HTTP/1.1 100 Continue\r\n\r\n";
    client.write((const uint8_t *)continueResponse,
                 sizeof(continueResponse) - 1);
  }

  body = (uint8_t *)malloc(bodyLength);
  if (!body) {
    error = "unable to allocate HTTP request body";
    return false;
  }

  size_t received = 0;
  lastData = millis();
  while (received < bodyLength) {
    const int available = client.available();
    if (available > 0) {
      const size_t want = min(bodyLength - received, (size_t)available);
      const int got = client.read(body + received, want);
      if (got > 0) {
        received += (size_t)got;
        lastData = millis();
        continue;
      }
    }

    if (!client.connected()) {
      error = "HTTP client disconnected before request body completed";
      free(body);
      body = nullptr;
      return false;
    }
    if (millis() - lastData > NETWORK_BODY_TIMEOUT_MS) {
      error = "HTTP request body timeout";
      free(body);
      body = nullptr;
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  return true;
}

struct ChunkTracker {
  enum State : uint8_t { SIZE_LINE, DATA, DATA_CRLF, TRAILERS, COMPLETE, INVALID };
  State state = SIZE_LINE;
  String line;
  size_t remaining = 0;
  uint8_t crlfBytes = 0;

  void feed(uint8_t byte) {
    if (state == COMPLETE || state == INVALID) return;

    if (state == SIZE_LINE) {
      if (byte == '\r') return;
      if (byte == '\n') {
        String sizeText = line;
        const int semi = sizeText.indexOf(';');
        if (semi >= 0) sizeText = sizeText.substring(0, semi);
        sizeText.trim();
        if (sizeText.isEmpty()) {
          state = INVALID;
          return;
        }
        char *end = nullptr;
        remaining = strtoul(sizeText.c_str(), &end, 16);
        if (!end || *end != '\0') {
          state = INVALID;
          return;
        }
        line = "";
        if (remaining == 0) state = TRAILERS;
        else state = DATA;
        return;
      }
      if (line.length() > 32) {
        state = INVALID;
        return;
      }
      line += (char)byte;
      return;
    }

    if (state == DATA) {
      if (remaining > 0) --remaining;
      if (remaining == 0) {
        state = DATA_CRLF;
        crlfBytes = 0;
      }
      return;
    }

    if (state == DATA_CRLF) {
      if (crlfBytes == 0 && byte != '\r') {
        state = INVALID;
        return;
      }
      if (crlfBytes == 1 && byte != '\n') {
        state = INVALID;
        return;
      }
      ++crlfBytes;
      if (crlfBytes == 2) {
        state = SIZE_LINE;
        line = "";
      }
      return;
    }

    if (state == TRAILERS) {
      if (byte == '\r') return;
      if (byte == '\n') {
        if (line.isEmpty()) state = COMPLETE;
        else line = "";
        return;
      }
      if (line.length() > 1024) {
        state = INVALID;
        return;
      }
      line += (char)byte;
    }
  }
};

static bool proxyUsbResponse(WiFiClient &network,
                             const String &method,
                             String &error) {
  uint8_t *headerBuffer = (uint8_t *)malloc(HTTP_HEADER_LIMIT);
  if (!headerBuffer) {
    error = "unable to allocate HTTP response header buffer";
    return false;
  }

  size_t used = 0;
  size_t headerEnd = 0;

  while (!headerEnd) {
    if (used >= HTTP_HEADER_LIMIT) {
      free(headerBuffer);
      error = "USB HTTP response header too large";
      return false;
    }

    size_t actual = 0;
    if (!scannerReadSome(headerBuffer + used,
                         min(USB_IO_CHUNK, HTTP_HEADER_LIMIT - used),
                         actual, USB_RESPONSE_TIMEOUT_MS, error)) {
      free(headerBuffer);
      return false;
    }
    if (!actual) continue;

    const size_t previous = used;
    used += actual;
    const size_t searchStart = previous > 3 ? previous - 3 : 0;
    for (size_t i = searchStart; i + 3 < used; ++i) {
      if (headerBuffer[i] == '\r' && headerBuffer[i + 1] == '\n' &&
          headerBuffer[i + 2] == '\r' && headerBuffer[i + 3] == '\n') {
        headerEnd = i + 4;
        break;
      }
    }
  }

  String rawHeader;
  rawHeader.reserve(headerEnd + 1);
  for (size_t i = 0; i < headerEnd; ++i) rawHeader += (char)headerBuffer[i];

  const int contentLength = headerContentLength(rawHeader);
  const bool chunked = headerIsChunked(rawHeader);
  const int status = responseStatusCode(rawHeader);
  const bool noBody = method == "HEAD" ||
                      (status >= 100 && status < 200) ||
                      status == 204 || status == 304;

  const String outgoingHeader = rewriteResponseHeader(rawHeader);
  bool networkAlive = writeNetwork(
      network, (const uint8_t *)outgoingHeader.c_str(), outgoingHeader.length());

  const uint8_t *initialBody = headerBuffer + headerEnd;
  size_t initialBodyLength = used - headerEnd;

  if (noBody) {
    free(headerBuffer);
    return true;
  }

  uint8_t usbBuffer[USB_IO_CHUNK];

  if (contentLength >= 0) {
    size_t remaining = (size_t)contentLength;

    const size_t first = min(initialBodyLength, remaining);
    if (first && networkAlive) {
      networkAlive = writeNetwork(network, initialBody, first);
    }
    remaining -= first;

    while (remaining > 0) {
      size_t actual = 0;
      if (!scannerReadSome(usbBuffer,
                           min(sizeof(usbBuffer), remaining),
                           actual, USB_RESPONSE_TIMEOUT_MS, error)) {
        free(headerBuffer);
        return false;
      }
      if (!actual) continue;

      const size_t take = min(actual, remaining);
      if (networkAlive) networkAlive = writeNetwork(network, usbBuffer, take);
      remaining -= take;

      if (actual > take) {
        Serial.printf("[SCAN] Warning: USB response exceeded Content-Length by %u bytes; discarded\n",
                      (unsigned)(actual - take));
      }
    }

    free(headerBuffer);
    return true;
  }

  if (chunked) {
    ChunkTracker tracker;

    for (size_t i = 0; i < initialBodyLength; ++i) tracker.feed(initialBody[i]);
    if (initialBodyLength && networkAlive) {
      networkAlive = writeNetwork(network, initialBody, initialBodyLength);
    }

    while (tracker.state != ChunkTracker::COMPLETE) {
      if (tracker.state == ChunkTracker::INVALID) {
        free(headerBuffer);
        error = "invalid chunked HTTP response from scanner";
        return false;
      }

      size_t actual = 0;
      if (!scannerReadSome(usbBuffer, sizeof(usbBuffer),
                           actual, USB_RESPONSE_TIMEOUT_MS, error)) {
        free(headerBuffer);
        return false;
      }
      if (!actual) continue;

      for (size_t i = 0; i < actual; ++i) tracker.feed(usbBuffer[i]);
      if (networkAlive) networkAlive = writeNetwork(network, usbBuffer, actual);
    }

    free(headerBuffer);
    return true;
  }

  free(headerBuffer);
  error = "scanner HTTP response has neither Content-Length nor chunked framing";
  return false;
}

static void handleScannerHttpClient(WiFiClient client) {
  g.busy = true;
  client.setNoDelay(true);
  client.setTimeout(15000);

  if (!g.ready) {
    sendLocalHttpError(client, 503, "Service Unavailable",
                       "USB eSCL scanner interface is not ready.");
    client.stop();
    g.busy = false;
    return;
  }

  String requestHeader;
  uint8_t *requestBody = nullptr;
  size_t requestBodyLength = 0;
  String error;

  if (!readNetworkRequest(client, requestHeader,
                          requestBody, requestBodyLength, error)) {
    sendLocalHttpError(client, 400, "Bad Request", error.c_str());
    client.stop();
    g.busy = false;
    return;
  }

  const String method = requestMethod(requestHeader);
  const String usbHeader = rewriteRequestHeader(requestHeader);

  Serial.printf("[SCAN] %s request -> USB IF=%u ALT=%u\n",
                method.c_str(), g.iface.interfaceNumber,
                g.iface.alternateSetting);

  bool ok = scannerWriteAll((const uint8_t *)usbHeader.c_str(),
                            usbHeader.length(), error);
  if (ok && requestBodyLength) {
    ok = scannerWriteAll(requestBody, requestBodyLength, error);
  }
  free(requestBody);
  requestBody = nullptr;

  if (!ok) {
    Serial.printf("[SCAN] USB request failed: %s\n", error.c_str());
    if (client.connected()) {
      sendLocalHttpError(client, 502, "Bad Gateway", error.c_str());
    }
    client.stop();
    g.busy = false;
    return;
  }

  if (!proxyUsbResponse(client, method, error)) {
    Serial.printf("[SCAN] USB HTTP response failed: %s\n", error.c_str());
    if (client.connected()) {
      sendLocalHttpError(client, 502, "Bad Gateway", error.c_str());
    }
  }

  client.stop();
  g.busy = false;
}

static bool networkAvailable() {
  return WiFi.status() == WL_CONNECTED ||
         WiFi.getMode() == WIFI_AP ||
         WiFi.getMode() == WIFI_AP_STA;
}

static void advertiseScannerIfReady() {
  if (!g.ready || g.mdnsAdvertised || WiFi.status() != WL_CONNECTED) return;

  if (!MDNS.addService("uscan", "tcp", UsbScannerBackend::NETWORK_PORT)) {
    return;
  }

  MDNS.addServiceTxt("uscan", "tcp", "txtvers", "1");
  MDNS.addServiceTxt("uscan", "tcp", "rs", "eSCL");
  MDNS.addServiceTxt("uscan", "tcp", "ty", "HP Smart Tank 520 Scanner");
  MDNS.addServiceTxt("uscan", "tcp", "pdl", "image/jpeg");
  MDNS.addServiceTxt("uscan", "tcp", "is", "platen");
  MDNS.addServiceTxt("uscan", "tcp", "duplex", "F");
  g.mdnsAdvertised = true;

  Serial.printf("[SCAN] AirScan discovery advertised: _uscan._tcp port %u rs=eSCL\n",
                UsbScannerBackend::NETWORK_PORT);
}

static void scannerProxyTask(void *) {
  static WiFiServer server(UsbScannerBackend::NETWORK_PORT);
  g.server = &server;

  while (true) {
    advertiseScannerIfReady();

    if (!g.serverStarted && networkAvailable()) {
      server.begin();
      server.setNoDelay(true);
      g.serverStarted = true;
      Serial.printf("[SCAN] eSCL HTTP proxy listening on TCP %u\n",
                    UsbScannerBackend::NETWORK_PORT);
    }

    if (g.serverStarted) {
      WiFiClient incoming = server.available();
      if (incoming) handleScannerHttpClient(incoming);
    }

    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

} // namespace

bool UsbScannerBackend::begin() {
  if (started_) return true;
  started_ = true;

  if (xTaskCreate(scannerUsbTask, "usb_escl_client",
                  8192, nullptr, 3, &g.usbTask) != pdPASS) {
    Serial.println("[SCAN] Could not start USB scanner client task; printing is unaffected");
    return false;
  }

  if (xTaskCreate(scannerProxyTask, "escl_http_proxy",
                  16384, nullptr, 1, &g.proxyTask) != pdPASS) {
    Serial.println("[SCAN] Could not start eSCL proxy task; printing is unaffected");
    return false;
  }

  return true;
}

bool UsbScannerBackend::ready() const {
  return g.ready;
}

bool UsbScannerBackend::busy() const {
  return g.busy;
}

uint8_t UsbScannerBackend::interfaceNumber() const {
  return g.iface.interfaceNumber;
}

uint8_t UsbScannerBackend::alternateSetting() const {
  return g.iface.alternateSetting;
}

uint8_t UsbScannerBackend::bulkOutEndpoint() const {
  return g.iface.bulkOut.address;
}

uint8_t UsbScannerBackend::bulkInEndpoint() const {
  return g.iface.bulkIn.address;
}

void ensureUsbScannerBackendStarted() {
  static bool attempted = false;
  static UsbScannerBackend scanner;
  if (attempted) return;
  attempted = true;
  if (!scanner.begin()) {
    Serial.println("[SCAN] Scanner service did not start; RAW printing remains available");
  }
}
