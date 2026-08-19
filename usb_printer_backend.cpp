#include "usb_printer_backend.h"
#include "status_led.h"
#include <WiFi.h>

namespace {
constexpr uint16_t RAW_PORT = 9100;
constexpr uint32_t RAW_IDLE_TIMEOUT_MS = 30000;
constexpr uint32_t RAW_JOB_DRAIN_MS = 1500;
constexpr uint32_t RAW_CLOSE_GUARD_MS = 250;
constexpr size_t RAW_RX_CHUNK = 8192;

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
  size_t accepted = 0;
  if (!host.bulkWrite(data, length, accepted, 10000, error)) return false;
  if (accepted != length) {
    error = "USB Bulk OUT short write: " + String((unsigned)accepted) + "/" + String((unsigned)length);
    return false;
  }
  return true;
}

void finishRawConnection(UsbPrinterBackend *backend, const char *reason) {
  // TCP FIN is not a USB printer completion indication. Give the printer time
  // to drain the final USB transfer before releasing the network connection.
  // No FF/PJL/UEL/form-feed is injected: RAW remains byte-for-byte transparent.
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
      rawClient.setTimeout(30000);
      rawLastDataMs = millis();
      rawBytesReceived = 0;
      Serial.println("[RAW] TCP 9100 client connected; transparent USB pass-through");
    }
    return;
  }

  size_t drained = 0;
  while (rawClient.available() && drained < sizeof(rawChunk)) {
    const size_t available = (size_t)rawClient.available();
    const size_t want = min(available, sizeof(rawChunk) - drained);
    const int got = rawClient.read(rawChunk + drained, want);
    if (got <= 0) break;
    drained += (size_t)got;
  }

  if (drained) {
    String error;
    if (!backend->sendDirect(rawChunk, drained, error)) {
      Serial.printf("[RAW] USB pass-through failed after %llu bytes: %s\n",
                    (unsigned long long)rawBytesReceived, error.c_str());
      rawClient.stop();
      rawBytesReceived = 0;
      return;
    }
    rawBytesReceived += drained;
    rawLastDataMs = millis();
  }

  if (!rawClient.connected()) {
    finishRawConnection(backend, "connection-closed");
  } else if (millis() - rawLastDataMs >= RAW_IDLE_TIMEOUT_MS) {
    finishRawConnection(backend, "idle-timeout");
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

  // Never complete a RAW job per 8 KiB chunk. Keep it PRINTING until the TCP
  // stream ends and the final USB pipeline drain has completed.
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
