#include "usb_printer_backend.h"
#include "status_led.h"
#include <WiFi.h>

namespace {
constexpr uint16_t RAW_PORT = 9100;
constexpr uint32_t RAW_IDLE_TIMEOUT_MS = 300000;
constexpr uint32_t RAW_JOB_DRAIN_MS = 3000;
constexpr uint32_t RAW_CLOSE_GUARD_MS = 250;
constexpr size_t RAW_RX_CHUNK = 4096;
constexpr size_t RAW_POLL_BUDGET = 16384;
constexpr size_t RAW_USB_CHUNK = 1024;
constexpr uint32_t RAW_USB_TIMEOUT_MS = 30000;

WiFiServer rawServer(RAW_PORT);
WiFiClient rawClient;
bool rawServerStarted = false;
unsigned long rawLastDataMs = 0;
uint64_t rawBytesReceived = 0;
bool rawTransportClosing = false;
unsigned long rawTransportCloseAtMs = 0;
static uint8_t rawChunk[RAW_RX_CHUNK];

bool selectedInterfaceUsable(const UsbHostManager &host) {
  const UsbPrinterInterfaceInfo *p = host.selectedInterface();
  return p && p->usableForRawPrint();
}

bool statusSaysReady(const UsbPrinterBackend &backend) {
  if (!backend.usbStatusValid()) return true;
  return backend.usbStatusSelected() &&
         !backend.usbPaperEmpty() &&
         !backend.usbStatusError();
}

const char *statusReasonText(const UsbPrinterBackend &backend) {
  if (!backend.usbStatusValid()) return "status-not-yet-available";
  if (backend.usbStatusError()) return "usb-printer-reports-error";
  if (backend.usbPaperEmpty()) return "usb-printer-reports-paper-empty";
  if (!backend.usbStatusSelected()) return "usb-printer-reports-not-selected";
  return "usb-printer-ready";
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
  if (rawTransportClosing) return;
  if (backend) backend->finishRawJob();

  // Keep the transport reserved for the same drain interval used by the
  // known-good blocking implementation. This prevents a phone/spooler from
  // opening a follow-up TCP 9100 connection while the backend is still in
  // PRINTING/draining state and having that new connection reset immediately.
  rawTransportClosing = true;
  rawTransportCloseAtMs = millis() + RAW_JOB_DRAIN_MS + RAW_CLOSE_GUARD_MS;

  Serial.printf("[RAW] TCP 9100 job ended (%s, %llu bytes); draining before socket release\n",
                reason, (unsigned long long)rawBytesReceived);
  rawBytesReceived = 0;
}

void serviceRawTransportClose() {
  if (!rawTransportClosing) return;
  if ((int32_t)(millis() - rawTransportCloseAtMs) < 0) return;

  if (rawClient) rawClient.stop();
  rawClient = WiFiClient();
  rawTransportClosing = false;
  rawTransportCloseAtMs = 0;
  Serial.println("[RAW] TCP 9100 socket released after drain guard");
}

void handleRawServer(UsbPrinterBackend *backend) {
  if (!rawServerStarted) return;

  // Do not dequeue another connection while the previous print job is still
  // inside its printer drain/close guard. The old implementation achieved the
  // same effect by blocking the whole loop for 3.25 seconds; this keeps the
  // loop responsive without resetting a legitimate follow-up client.
  if (rawTransportClosing) {
    serviceRawTransportClose();
    return;
  }

  if (!rawClient) {
    WiFiClient incoming = rawServer.available();
    if (incoming) {
      if (!backend->online()) {
        Serial.printf("[RAW] Printer not ready; rejecting TCP 9100 connection: %s\n",
                      backend->statusReason().c_str());
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

  bool movedData = false;
  size_t movedThisPoll = 0;
  while (rawClient.available() > 0 && movedThisPoll < RAW_POLL_BUDGET) {
    const size_t available = (size_t)rawClient.available();
    const size_t budgetLeft = RAW_POLL_BUDGET - movedThisPoll;
    const size_t want = min(min(available, sizeof(rawChunk)), budgetLeft);
    const int got = rawClient.read(rawChunk, want);
    if (got <= 0) break;

    String error;
    if (!backend->sendDirect(rawChunk, (size_t)got, error)) {
      Serial.printf("[RAW] USB pass-through failed after %llu bytes: %s\n",
                    (unsigned long long)rawBytesReceived, error.c_str());
      backend->abortRawJob(error);
      rawClient.stop();
      rawClient = WiFiClient();
      rawBytesReceived = 0;
      return;
    }

    rawBytesReceived += (size_t)got;
    movedThisPoll += (size_t)got;
    rawLastDataMs = millis();
    movedData = true;
    yield();
  }

  const bool closed = !rawClient.connected();
  const bool pending = rawClient.available() > 0;
  if (closed && !pending) {
    finishRawConnection(backend, "connection-closed-and-drained");
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

bool UsbPrinterBackend::rawClientConnected() const {
  return (bool)rawClient && !rawTransportClosing;
}

bool UsbPrinterBackend::begin() {
  StatusLed::begin();
  StatusLed::set(StatusLed::BOOT);
  if (!host_.begin()) {
    state_ = ERROR;
    reason_ = host_.lastError();
    StatusLed::set(StatusLed::ERROR);
    return false;
  }
  state_ = OFFLINE;
  reason_ = "waiting-for-usb-printer";
  jobBytes_ = 0;
  drainPending_ = false;
  return true;
}

void UsbPrinterBackend::completeDrainIfReady() {
  if (!drainPending_ || state_ != PRINTING) return;
  if ((int32_t)(millis() - drainUntilMs_) < 0) return;

  drainPending_ = false;
  if (usbStatusValid() && usbStatusError()) {
    state_ = ERROR;
    reason_ = "usb-printer-reports-error-after-job";
    StatusLed::set(StatusLed::ERROR);
  } else if (usbStatusValid() && (usbPaperEmpty() || !usbStatusSelected())) {
    state_ = OFFLINE;
    reason_ = statusReasonText(*this);
    StatusLed::set(StatusLed::WAITING_FOR_PRINTER);
  } else {
    state_ = IDLE;
    reason_ = usbStatusValid() ? "usb-printer-ready" : "printer-interface-ready";
    StatusLed::set(StatusLed::PRINTER_READY);
  }

  Serial.printf("[RAW] USB transport complete: %llu bytes; final USB status=%s%s%s%s\n",
                (unsigned long long)jobBytes_,
                usbStatusValid() ? "valid" : "unavailable",
                usbStatusValid() ? " selected=" : "",
                usbStatusValid() ? (usbStatusSelected() ? "yes" : "no") : "",
                usbStatusValid() ? (usbStatusError() ? " error=yes" : " error=no") : "");
  jobBytes_ = 0;
}

void UsbPrinterBackend::poll() {
  startRawServerIfNeeded();
  completeDrainIfReady();
  handleRawServer(this);

  if (state_ == PRINTING) {
    StatusLed::set(StatusLed::PRINTING);
    StatusLed::update();
    return;
  }

  switch (host_.state()) {
    case UsbHostManager::PRINTER_READY:
      if (!selectedInterfaceUsable(host_)) {
        state_ = ERROR;
        reason_ = "selected-interface-has-no-bulk-output";
      } else if (!statusSaysReady(*this)) {
        state_ = usbStatusError() ? ERROR : OFFLINE;
        reason_ = statusReasonText(*this);
      } else {
        state_ = IDLE;
        reason_ = usbStatusValid() ? "usb-printer-ready" : "printer-interface-ready";
      }
      StatusLed::set(state_ == IDLE ? StatusLed::PRINTER_READY :
                     (state_ == ERROR ? StatusLed::ERROR : StatusLed::WAITING_FOR_PRINTER));
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
    error = "Selected USB interface has no usable RAW Bulk OUT endpoint";
    return false;
  }
  if (state_ != IDLE && state_ != PRINTING) {
    error = reason_;
    return false;
  }

  if (state_ == IDLE) {
    jobBytes_ = 0;
    drainPending_ = false;
  }
  state_ = PRINTING;
  if (!sendUsbChunk(host_, data, length, error)) {
    abortRawJob(error);
    return false;
  }
  jobBytes_ += length;
  reason_ = "raw-job-in-progress";
  return true;
}

void UsbPrinterBackend::finishRawJob() {
  if (state_ != PRINTING || drainPending_) return;
  drainPending_ = true;
  drainUntilMs_ = millis() + RAW_JOB_DRAIN_MS;
  reason_ = "raw-job-draining";
}

void UsbPrinterBackend::abortRawJob(const String &reason) {
  drainPending_ = false;
  drainUntilMs_ = 0;
  jobBytes_ = 0;
  state_ = ERROR;
  reason_ = reason.length() ? reason : "raw-job-aborted";
  StatusLed::set(StatusLed::ERROR);
}