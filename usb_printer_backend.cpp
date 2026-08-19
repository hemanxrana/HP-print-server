#include "usb_printer_backend.h"
#include "status_led.h"
#include <WiFi.h>

namespace {
constexpr uint16_t RAW_PORT = 9100;
constexpr uint32_t RAW_IDLE_TIMEOUT_MS = 300000;
constexpr uint32_t RAW_JOB_DRAIN_MS = 3000;
constexpr uint32_t RAW_CLOSE_GUARD_MS = 250;
constexpr size_t RAW_RX_CHUNK = 4096;
constexpr size_t RAW_USB_CHUNK = 1024;
constexpr uint32_t RAW_USB_TIMEOUT_MS = 30000;

WiFiServer rawServer(RAW_PORT);
WiFiClient rawClient;
bool rawServerStarted = false;
unsigned long rawLastDataMs = 0;
uint64_t rawBytesReceived = 0;
static uint8_t rawChunk[RAW_RX_CHUNK];

bool selectedInterfaceUsable(const UsbHostManager &host) {
  const UsbPrinterInterfaceInfo *p = host.selectedInterface();
  return p && p->usableForRawPrint();
}

bool sendUsbChunk(UsbHostManager &host, const uint8_t *data, size_t length, String &error) {
  if (!data || !length) return true;
  size_t offset = 0;
  while (offset < length) {
    const size_t part = min(RAW_USB_CHUNK, length - offset);
    size_t accepted = 0;
    if (!host.bulkWrite(data + offset, part, accepted, RAW_USB_TIMEOUT_MS, error)) return false;
    if (accepted != part) {
      error = "USB Bulk OUT short write: " + String((unsigned)accepted) + "/" + String((unsigned)part);
      return false;
    }
    offset += part;
    yield();
  }
  return true;
}

void finishRawConnection(UsbPrinterBackend *backend, const char *reason) {
  if (backend) backend->finishRawJob();
  delay(RAW_CLOSE_GUARD_MS);
  if (rawClient) rawClient.stop();
  Serial.printf("[RAW] TCP 9100 job ended (%s, %llu bytes)\n",
                reason, (unsigned long long)rawBytesReceived);
  rawBytesReceived = 0;
}

void handleRawServer(UsbPrinterBackend *backend) {
  if (!rawServerStarted) return;

  if (!rawClient) {
    WiFiClient incoming = rawServer.available();
    if (incoming) {
      if (!backend->online()) {
        Serial.println("[RAW] Printer not ready; rejecting TCP 9100 connection");
        incoming.stop();
        return;
      }
      rawClient = incoming;
      rawClient.setNoDelay(true);
      rawClient.setTimeout(RAW_IDLE_TIMEOUT_MS);
      rawLastDataMs = millis();
      rawBytesReceived = 0;
      Serial.println("[RAW] TCP 9100 client connected; transparent USB pass-through");
    }
    return;
  }

  // TCP connected() may become false immediately after the sender sends FIN,
  // while bytes already received by lwIP are still waiting in the RX buffer.
  // Therefore a closed socket is NOT a completion indication until every
  // currently buffered byte has been forwarded to USB.
  bool movedData = false;
  while (rawClient.available() > 0) {
    const size_t available = (size_t)rawClient.available();
    const size_t want = min(available, sizeof(rawChunk));
    const int got = rawClient.read(rawChunk, want);
    if (got <= 0) break;

    String error;
    if (!backend->sendDirect(rawChunk, (size_t)got, error)) {
      Serial.printf("[RAW] USB pass-through failed after %llu bytes: %s\n",
                    (unsigned long long)rawBytesReceived, error.c_str());
      rawClient.stop();
      rawBytesReceived = 0;
      return;
    }

    rawBytesReceived += (size_t)got;
    rawLastDataMs = millis();
    movedData = true;
    yield();
  }

  const bool closed = !rawClient.connected();
  const bool pending = rawClient.available() > 0;

  // Only finish after the TCP peer is closed AND the ESP32 socket has no data
  // left to drain. This fixes the previous end-of-job data-loss window.
  if (closed && !pending) {
    finishRawConnection(backend, "connection-closed");
  } else if (!movedData && millis() - rawLastDataMs >= RAW_IDLE_TIMEOUT_MS) {
    finishRawConnection(backend, "idle-timeout-5min");
  }
}

void startRawServerIfNeeded() {
  if (WiFi.status() != WL_CONNECTED &&
      WiFi.getMode() != WIFI_AP &&
      WiFi.getMode() != WIFI_AP_STA) return;
  if (!rawServerStarted) {
    rawServer.begin();
    rawServer.setNoDelay(true);
    rawServerStarted = true;
    Serial.println("[RAW] JetDirect/AppSocket listening on TCP 9100");
  }
}
} // namespace

bool UsbPrinterBackend::begin() {
  StatusLed::begin();
  StatusLed::set(StatusLed::BOOT);
  if (!host_.begin()) {
    configured_ = false;
    state_ = OFFLINE;
    reason_ = host_.lastError();
    StatusLed::set(StatusLed::ERROR);
    return false;
  }
  configured_ = true;
  state_ = OFFLINE;
  reason_ = "waiting-for-usb-printer";
  return true;
}

void UsbPrinterBackend::poll() {
  startRawServerIfNeeded();
  handleRawServer(this);

  if (!configured_) {
    if (host_.state() == UsbHostManager::PRINTER_READY) {
      configured_ = true;
      state_ = selectedInterfaceUsable(host_) ? IDLE : ERROR;
      reason_ = selectedInterfaceUsable(host_) ? "printer-interface-ready" : "selected-interface-has-no-bulk-output";
      StatusLed::begin();
      StatusLed::set(state_ == IDLE ? StatusLed::PRINTER_READY : StatusLed::ERROR);
    } else {
      StatusLed::update();
      return;
    }
  }

  if (state_ == PRINTING) {
    StatusLed::set(StatusLed::PRINTING);
    StatusLed::update();
    return;
  }

  switch (host_.state()) {
    case UsbHostManager::PRINTER_READY:
      state_ = selectedInterfaceUsable(host_) ? IDLE : ERROR;
      reason_ = selectedInterfaceUsable(host_) ? "printer-interface-ready" : "selected-interface-has-no-bulk-output";
      StatusLed::set(state_ == IDLE ? StatusLed::PRINTER_READY : StatusLed::ERROR);
      break;
    case UsbHostManager::DEVICE_ATTACHED:
    case UsbHostManager::ENUMERATING:
      state_ = OFFLINE;
      reason_ = "enumerating-usb-device";
      StatusLed::set(StatusLed::WAITING_FOR_PRINTER);
      break;
    case UsbHostManager::ERROR:
      state_ = ERROR;
      reason_ = host_.lastError();
      StatusLed::set(StatusLed::ERROR);
      break;
    default:
      state_ = OFFLINE;
      reason_ = host_.lastError().length() ? host_.lastError() : "waiting-for-usb-printer";
      StatusLed::set(StatusLed::WAITING_FOR_PRINTER);
      break;
  }
  StatusLed::update();
}

bool UsbPrinterBackend::sendDirect(const uint8_t *data, size_t length, String &error) {
  if (!data || !length) {
    error = "Empty print data";
    return false;
  }
  const UsbPrinterInterfaceInfo *p = host_.selectedInterface();
  if (!p || !p->usableForRawPrint()) {
    error = "Selected USB interface has no usable Bulk OUT endpoint";
    return false;
  }
  if (state_ != IDLE && state_ != PRINTING) {
    error = reason_;
    return false;
  }

  state_ = PRINTING;
  if (!sendUsbChunk(host_, data, length, error)) {
    state_ = ERROR;
    reason_ = error;
    StatusLed::set(StatusLed::ERROR);
    return false;
  }
  reason_ = "raw-job-in-progress";
  return true;
}

void UsbPrinterBackend::finishRawJob() {
  if (state_ == PRINTING) {
    delay(RAW_JOB_DRAIN_MS);
    state_ = IDLE;
    reason_ = "printer-interface-ready";
    StatusLed::set(StatusLed::PRINTER_READY);
  }
}
