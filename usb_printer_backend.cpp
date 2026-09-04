#include "usb_printer_backend.h"
#include "status_led.h"
#include <WiFi.h>
#include <errno.h>
#include <lwip/sockets.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
constexpr uint16_t RAW_PORT = 9100;
constexpr uint32_t RAW_IDLE_TIMEOUT_MS = 300000;
constexpr uint32_t RAW_JOB_DRAIN_MS = 8000;
constexpr uint32_t POST_JOB_STATUS_WAIT_MAX_MS = 15000;
constexpr uint64_t HEALTH_LOG_INTERVAL_BYTES = 512ULL * 1024ULL;
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
uint32_t rawJobSequence = 0;
uint32_t rawActiveJobId = 0;
bool rawTransportClosing = false;
bool rawCloseGuardStarted = false;
unsigned long rawTransportCloseAtMs = 0;
static uint8_t rawChunk[RAW_RX_CHUNK];

enum class RawPeerState : uint8_t {
  OPEN,
  CLEAN_FIN,
  ERROR
};

bool rawClientAllocated() {
  return rawClient.fd() >= 0;
}

RawPeerState rawPeerState(int &socketError) {
  socketError = 0;
  const int fd = rawClient.fd();
  if (fd < 0) {
    socketError = ENOTCONN;
    return RawPeerState::ERROR;
  }

  uint8_t byte = 0;
  errno = 0;
  const int result = recv(fd, &byte, 1, MSG_DONTWAIT | MSG_PEEK);

  if (result == 0) return RawPeerState::CLEAN_FIN;
  if (result > 0) return RawPeerState::OPEN;

  socketError = errno;
  switch (socketError) {
    case EWOULDBLOCK:
#if EAGAIN != EWOULDBLOCK
    case EAGAIN:
#endif
    case ENOENT:
      return RawPeerState::OPEN;

    case ENOTCONN:
    case EPIPE:
    case ECONNRESET:
    case ECONNREFUSED:
    case ECONNABORTED:
      return RawPeerState::ERROR;

    default:
      return RawPeerState::ERROR;
  }
}

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

void logTransportHealth(uint64_t bytes) {
  Serial.printf("[PRINT][HEALTH] bytes=%llu free-heap=%lu min-free-heap=%lu largest-block=%u loop-stack-watermark=%u words\n",
                (unsigned long long)bytes,
                (unsigned long)ESP.getFreeHeap(),
                (unsigned long)ESP.getMinFreeHeap(),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                (unsigned)uxTaskGetStackHighWaterMark(nullptr));
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

void resetRawTransportState() {
  rawClient = WiFiClient();
  rawBytesReceived = 0;
  rawActiveJobId = 0;
  rawTransportClosing = false;
  rawCloseGuardStarted = false;
  rawTransportCloseAtMs = 0;
}

void abortRawConnection(UsbPrinterBackend *backend, const String &reason) {
  const uint32_t jobId = rawActiveJobId;
  const uint64_t bytes = rawBytesReceived;

  if (backend && bytes > 0) backend->abortRawJob(reason);
  if (rawClientAllocated()) rawClient.stop();

  Serial.printf("[RAW] Job #%u INCOMPLETE after %llu USB-confirmed bytes: %s\n",
                jobId, (unsigned long long)bytes,
                reason.length() ? reason.c_str() : "unknown transport failure");
  resetRawTransportState();
}

void finishRawConnection(UsbPrinterBackend *backend, const char *reason) {
  if (rawTransportClosing) return;

  if (rawBytesReceived == 0) {
    Serial.printf("[RAW] TCP 9100 connection #%u closed without print data (%s)\n",
                  rawActiveJobId, reason);
    if (rawClientAllocated()) rawClient.stop();
    resetRawTransportState();
    return;
  }

  if (backend) backend->finishRawJob();
  rawTransportClosing = true;
  rawCloseGuardStarted = false;
  rawTransportCloseAtMs = 0;

  Serial.printf("[RAW] Job #%u input complete (%s): %llu bytes USB-confirmed; waiting for printer drain\n",
                rawActiveJobId, reason,
                (unsigned long long)rawBytesReceived);
}

void serviceRawTransportClose(UsbPrinterBackend *backend) {
  if (!rawTransportClosing) return;
  if (backend && backend->state() == UsbPrinterBackend::PRINTING) return;

  if (!rawCloseGuardStarted) {
    rawCloseGuardStarted = true;
    rawTransportCloseAtMs = millis() + RAW_CLOSE_GUARD_MS;
    Serial.printf("[RAW] Job #%u USB drain complete; applying %lu ms socket close guard\n",
                  rawActiveJobId, (unsigned long)RAW_CLOSE_GUARD_MS);
    return;
  }

  if ((int32_t)(millis() - rawTransportCloseAtMs) < 0) return;

  const uint32_t jobId = rawActiveJobId;
  const uint64_t bytes = rawBytesReceived;
  const bool backendReady = !backend || backend->state() == UsbPrinterBackend::IDLE;
  const String finalReason = backend ? backend->statusReason() : String();

  if (rawClientAllocated()) rawClient.stop();

  Serial.printf("[RAW] Job #%u COMPLETE: %llu bytes received and accepted by USB; printer-state=%s%s%s\n",
                jobId, (unsigned long long)bytes,
                backendReady ? "ready" : "not-ready",
                finalReason.length() ? " reason=" : "",
                finalReason.length() ? finalReason.c_str() : "");
  resetRawTransportState();
}

void handleRawServer(UsbPrinterBackend *backend) {
  if (!rawServerStarted) return;

  if (rawTransportClosing) {
    serviceRawTransportClose(backend);
    return;
  }

  if (!rawClientAllocated()) {
    WiFiClient incoming = rawServer.available();
    if (incoming.fd() >= 0) {
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
      rawActiveJobId = ++rawJobSequence;
      Serial.printf("[RAW] Job #%u TCP 9100 client connected; transparent USB pass-through\n",
                    rawActiveJobId);
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
      Serial.printf("[RAW] Job #%u USB pass-through failed after %llu bytes: %s\n",
                    rawActiveJobId,
                    (unsigned long long)rawBytesReceived,
                    error.c_str());
      abortRawConnection(backend, error);
      return;
    }

    rawBytesReceived += (size_t)got;
    movedThisPoll += (size_t)got;
    rawLastDataMs = millis();
    movedData = true;
    yield();
  }

  const bool pending = rawClient.available() > 0;
  int peerError = 0;
  const RawPeerState peer = rawPeerState(peerError);

  if (peer == RawPeerState::CLEAN_FIN && !pending) {
    finishRawConnection(backend, "clean-peer-FIN-and-buffer-drained");
    return;
  }

  if (peer == RawPeerState::ERROR && !pending) {
    abortRawConnection(backend,
                       String("TCP peer ended without clean FIN; errno=") + String(peerError));
    return;
  }

  if (!movedData && millis() - rawLastDataMs >= RAW_IDLE_TIMEOUT_MS) {
    if (rawBytesReceived == 0) {
      Serial.printf("[RAW] TCP 9100 connection #%u idle with no print data; closing\n",
                    rawActiveJobId);
      if (rawClientAllocated()) rawClient.stop();
      resetRawTransportState();
    } else {
      abortRawConnection(backend, "TCP client stalled before sending job FIN");
    }
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
  return rawClientAllocated() || rawTransportClosing;
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
  nextHealthLogAt_ = HEALTH_LOG_INTERVAL_BYTES;
  drainPending_ = false;
  drainStartedMs_ = 0;
  drainStatusAtStart_ = 0;
  return true;
}

void UsbPrinterBackend::completeDrainIfReady() {
  if (!drainPending_ || state_ != PRINTING) return;
  if (usbStatusValid() && usbStatusError()) {
    drainPending_ = false; state_ = ERROR; reason_ = "usb-printer-reports-error-after-job";
    StatusLed::set(StatusLed::ERROR);
    Serial.printf("[PRINT][FINALIZE] printer reported error after %llu USB-confirmed bytes\n", (unsigned long long)jobBytes_);
    jobBytes_ = 0; return;
  }
  if (usbStatusValid() && (usbPaperEmpty() || !usbStatusSelected())) {
    drainPending_ = false; state_ = OFFLINE; reason_ = statusReasonText(*this);
    StatusLed::set(StatusLed::WAITING_FOR_PRINTER);
    Serial.printf("[PRINT][FINALIZE] printer not ready after job: %s\n", reason_.c_str());
    jobBytes_ = 0; return;
  }

  const unsigned long now = millis();
  if ((int32_t)(now - drainUntilMs_) < 0) return;
  const bool statusUnavailable = !usbStatusValid();
  const bool freshStatus = usbStatusValid() && host_.portStatus().updatedAt > drainStatusAtStart_;
  if (!statusUnavailable && !freshStatus && now - drainStartedMs_ < POST_JOB_STATUS_WAIT_MAX_MS) return;

  drainPending_ = false; state_ = IDLE;
  reason_ = freshStatus ? "usb-printer-ready-after-fresh-status" : "printer-ready-after-post-job-guard";
  StatusLed::set(StatusLed::PRINTER_READY);
  Serial.printf("[PRINT][FINALIZE] transport complete: %llu bytes; waited=%lu ms; status=%s fresh=%s value=0x%02X\n",
                (unsigned long long)jobBytes_, (unsigned long)(now - drainStartedMs_),
                usbStatusValid() ? "valid" : "unavailable", freshStatus ? "yes" : "no",
                usbStatusValid() ? usbPortStatus() : 0);
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
    nextHealthLogAt_ = HEALTH_LOG_INTERVAL_BYTES;
    drainPending_ = false;
  }
  state_ = PRINTING;
  if (!sendUsbChunk(host_, data, length, error)) {
    abortRawJob(error);
    return false;
  }
  jobBytes_ += length;
  if (jobBytes_ >= nextHealthLogAt_) {
    logTransportHealth(jobBytes_);
    while (nextHealthLogAt_ <= jobBytes_) nextHealthLogAt_ += HEALTH_LOG_INTERVAL_BYTES;
  }
  reason_ = "raw-job-in-progress";
  return true;
}

void UsbPrinterBackend::finishRawJob() {
  if (state_ != PRINTING || drainPending_) return;
  drainPending_ = true;
  drainStartedMs_ = millis();
  drainUntilMs_ = drainStartedMs_ + RAW_JOB_DRAIN_MS;
  drainStatusAtStart_ = usbStatusValid() ? host_.portStatus().updatedAt : 0;
  reason_ = "raw-job-draining";
  Serial.printf("[PRINT][FINALIZE] last USB document byte accepted; guard=%lu ms status-at-start=%lu\n",
                (unsigned long)RAW_JOB_DRAIN_MS, (unsigned long)drainStatusAtStart_);
}

void UsbPrinterBackend::abortRawJob(const String &reason) {
  drainPending_ = false;
  drainUntilMs_ = 0;
  jobBytes_ = 0;
  state_ = ERROR;
  reason_ = reason.length() ? reason : "raw-job-aborted";
  StatusLed::set(StatusLed::ERROR);
}