#include "usb_printer_backend.h"
#include "status_led.h"
#include <WiFi.h>

namespace {
constexpr uint16_t RAW_PORT = 9100;
constexpr uint32_t RAW_IDLE_TIMEOUT_MS = 30000;
constexpr uint32_t RAW_JOB_DRAIN_MS = 2500;
constexpr uint32_t RAW_CLOSE_GUARD_MS = 500;
constexpr size_t RAW_RX_CHUNK = 8192;

WiFiServer rawServer(RAW_PORT);
WiFiClient rawClient;
bool rawServerStarted = false;
unsigned long rawLastDataMs = 0;
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
  if (!backend) return;

  // TCP close only tells us the network sender is finished. The printer can
  // still be consuming USB data. Keep the backend in PRINTING while the USB
  // pipeline drains, then take a real Printer Class status sample.
  backend->finishRawJob();
  delay(RAW_CLOSE_GUARD_MS);
  if (rawClient) rawClient.stop();
  backend->setRawClientConnected(false);
  Serial.printf("[RAW] TCP 9100 job ended (%s, %llu bytes)\n",
                reason, (unsigned long long)backend->rawBytesReceived());
  backend->clearRawBytes();
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
      backend->clearRawBytes();
      backend->setRawClientConnected(true);
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
                    (unsigned long long)backend->rawBytesReceived(), error.c_str());
      rawClient.stop();
      backend->setRawClientConnected(false);
      backend->clearRawBytes();
      return;
    }
    backend->addRawBytes(drained);
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

  // Keep the cached USB Printer Class status fresh. The request itself is
  // executed by the USB host client task, not by the Arduino loop thread.
  static unsigned long lastUsbStatus = 0;
  if (millis() - lastUsbStatus >= 1000) {
    lastUsbStatus = millis();
    host_.requestStatusRefresh();
  }

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

  // A valid USB Printer Class status is authoritative for the physical
  // printer: bit 5 = paper empty, bit 4 = selected, bit 3 = no error.
  if (host_.statusValid()) {
    const uint8_t s = host_.portStatus();
    if (!(s & 0x08)) {
      state_ = ERROR;
      reason_ = (s & 0x20) ? "printer-reports-paper-empty-and-error" : "printer-reports-usb-error";
      StatusLed::set(StatusLed::ERROR);
      StatusLed::update();
      return;
    }
    if (!(s & 0x10)) {
      state_ = OFFLINE;
      reason_ = "printer-not-selected";
      StatusLed::set(StatusLed::WAITING_FOR_PRINTER);
      StatusLed::update();
      return;
    }
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
  if (state_ != PRINTING) return;

  // Do not declare completion merely because the TCP sender disconnected.
  // Wait for the USB pipeline to drain and sample the printer status several
  // times. No bytes are added to the print stream and no USB soft-reset is
  // issued, because either would risk truncating the final page.
  delay(RAW_JOB_DRAIN_MS);
  for (uint8_t i = 0; i < 3; ++i) {
    host_.requestStatusRefresh();
    delay(200);
  }

  if (host_.statusValid() && !(host_.portStatus() & 0x08)) {
    state_ = ERROR;
    reason_ = (host_.portStatus() & 0x20)
                ? "printer-reports-paper-empty-and-error"
                : "printer-reports-usb-error-after-job";
    StatusLed::set(StatusLed::ERROR);
    return;
  }

  state_ = IDLE;
  reason_ = "printer-interface-ready";
  StatusLed::set(StatusLed::PRINTER_READY);
}
