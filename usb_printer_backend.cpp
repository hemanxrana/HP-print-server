#include "usb_printer_backend.h"
#include "logger.h"
#include "status_led.h"
#include "job_log.h"
#include <WiFi.h>
#include <errno.h>
#include <string.h>
#include <lwip/sockets.h>
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "config.h"

// Map config.h names to local names used in this file
#define RAW_USB_CHUNK BULK_OUT_CHUNK_SIZE
#define RAW_RX_CHUNK BULK_OUT_CHUNK_SIZE
#define RAW_POLL_BUDGET USB_BUFFER_SIZE
constexpr uint32_t RAW_USB_TIMEOUT_MS = 30000;

namespace {
constexpr uint16_t RAW_PORT = RAW_SOCKET_PORT;
constexpr uint32_t POST_JOB_LOG_INTERVAL_MS = 2000;
constexpr uint64_t HEALTH_LOG_INTERVAL_BYTES = 512ULL * 1024ULL;
constexpr uint32_t RAW_CLOSE_GUARD_MS = 250;
constexpr uint32_t RAW_PROBE_IDLE_TIMEOUT_MS = 30000;
constexpr uint32_t RAW_IDLE_TIMEOUT_MS = 60000;
constexpr bool RAW_APPEND_JOB_TERMINATOR = false;
constexpr char RAW_JOB_TERMINATOR[] = "\x1b%-12345X@PJL EOJ\r\n\x0c";
constexpr bool RAW_APPEND_FORM_FEED = false;
constexpr char RAW_FORM_FEED[] = "\x0c";
// RAW job terminator: Universal Exit Language immediately followed by
// "@PJL EOJ".  Matching a bare "@PJL EOJ" anywhere in the stream risked
// truncating a PCL raster that happened to contain those ASCII bytes.  A real
// terminator always starts with UEL (ESC %-12345X); if a driver omits it the
// job still finishes when the TCP client closes, so a missed match is safe
// where a false positive is not.
constexpr uint8_t PJL_EOJ_PATTERN[] = {
  0x1B, '%', '-', '1', '2', '3', '4', '5', 'X', '@', 'P', 'J', 'L', ' ', 'E', 'O', 'J'
};
constexpr size_t PJL_EOJ_PATTERN_LEN = sizeof(PJL_EOJ_PATTERN);

struct PjlDetector {
  uint8_t window[PJL_EOJ_PATTERN_LEN] = {0};
  size_t pos = 0;
  bool filled = false;

  bool feed(uint8_t b) {
    window[pos] = b;
    pos = (pos + 1) % PJL_EOJ_PATTERN_LEN;
    if (pos == 0) filled = true;
    // Compare only when the newest byte is the pattern's last byte, so the
    // steady-state cost is one compare per received byte.
    if (!filled || b != 'J') return false;
    for (size_t i = 0; i < PJL_EOJ_PATTERN_LEN; ++i) {
      if (window[(pos + i) % PJL_EOJ_PATTERN_LEN] != PJL_EOJ_PATTERN[i]) return false;
    }
    return true;
  }
  void reset() {
    pos = 0;
    filled = false;
    memset(window, 0, sizeof(window));
  }
};

WiFiServer rawServer(RAW_PORT);
WiFiClient rawClient;
bool rawServerStarted = false;
unsigned long rawLastDataMs = 0;
uint64_t rawBytesReceived = 0;
uint32_t rawJobSequence = 0;
uint32_t rawActiveJobId = 0;
unsigned long rawJobStartedMs = 0;
String rawClientIp;
bool rawJobLogged = false;
bool rawTransportClosing = false;
bool rawCloseGuardStarted = false;
unsigned long rawTransportCloseAtMs = 0;
static uint8_t *rawChunk = nullptr;
static PjlDetector pjlDetector;
static uint32_t ringWritePos = 0;
static uint32_t ringReadPos = 0;
static uint32_t ringDataLen = 0;

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

void logTransportHealth(uint64_t bytes, uint32_t elapsedMs) {
  const double kb = bytes / 1024.0;
  const double rate = elapsedMs ? (kb * 1000.0 / elapsedMs) : 0.0;
  Log.printf("[PRINT][RATE] bytes=%llu elapsed=%lu ms avg=%.1f KB/s free-heap=%lu min-free-heap=%lu largest-block=%u loop-stack-watermark=%u words\n",
                (unsigned long long)bytes, (unsigned long)elapsedMs, rate,
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
  return host.bulkWriteAll(data, length, RAW_USB_TIMEOUT_MS, error);
}

void resetRawTransportState() {
  rawClient = WiFiClient();
  rawBytesReceived = 0;
  rawActiveJobId = 0;
  rawJobStartedMs = 0;
  rawClientIp = "";
  rawJobLogged = false;
  rawTransportClosing = false;
  rawCloseGuardStarted = false;
  rawTransportCloseAtMs = 0;
  ringWritePos = 0;
  ringReadPos = 0;
  ringDataLen = 0;
  pjlDetector.reset();
}

void abortRawConnection(UsbPrinterBackend *backend, const String &reason) {
  const uint32_t jobId = rawActiveJobId;
  const uint64_t bytes = rawBytesReceived;

  if (backend && bytes > 0) backend->abortRawJob(reason);
  if (rawClientAllocated()) rawClient.stop();

  Log.printf("[RAW] Job #%lu INCOMPLETE after %llu USB-confirmed bytes: %s\n",
                (unsigned long)jobId, (unsigned long long)bytes,
                reason.length() ? reason.c_str() : "unknown transport failure");
  if (bytes > 0) {
    JobLog::record(JobLog::RAW, jobId, 8, reason.c_str(),
                   "raw-9100-aborted", (uint32_t)bytes,
                   rawJobStartedMs ? (uint32_t)(millis() - rawJobStartedMs) : 0,
                   rawClientIp.c_str(), "raw");
  }
  resetRawTransportState();
}

void finishRawConnection(UsbPrinterBackend *backend, const char *reason) {
  if (rawTransportClosing) return;

  if (rawBytesReceived == 0) {
    Log.printf("[RAW] TCP 9100 connection #%lu closed without print data (%s)\n",
                  (unsigned long)rawActiveJobId, reason);
    if (rawClientAllocated()) rawClient.stop();
    resetRawTransportState();
    return;
  }

  if (RAW_APPEND_JOB_TERMINATOR && backend) {
    String terminatorError;
    if (!backend->sendDirect((const uint8_t *)RAW_JOB_TERMINATOR, strlen(RAW_JOB_TERMINATOR), terminatorError)) {
      abortRawConnection(backend, String("job terminator failed: ") + terminatorError);
      return;
    }
  }
  if (RAW_APPEND_FORM_FEED && backend) {
    String ffError;
    if (!backend->sendDirect((const uint8_t *)RAW_FORM_FEED, strlen(RAW_FORM_FEED), ffError)) {
      abortRawConnection(backend, String("form-feed failed: ") + ffError);
      return;
    }
  }
  if (backend) backend->finishRawJob();
  rawTransportClosing = true;
  rawCloseGuardStarted = false;
  rawTransportCloseAtMs = 0;

  Log.printf("[RAW] Job #%lu input complete (%s): %llu bytes USB-confirmed; waiting for printer drain\n",
                (unsigned long)rawActiveJobId, reason,
                (unsigned long long)rawBytesReceived);
}

void serviceRawTransportClose(UsbPrinterBackend *backend) {
  (void)backend;
  if (!rawTransportClosing) return;

  if (!rawCloseGuardStarted) {
    rawCloseGuardStarted = true;
    rawTransportCloseAtMs = millis() + RAW_CLOSE_GUARD_MS;
    Log.printf("[RAW] Job #%lu applying %lu ms socket close guard\n",
                  (unsigned long)rawActiveJobId, (unsigned long)RAW_CLOSE_GUARD_MS);
    return;
  }

  if ((int32_t)(millis() - rawTransportCloseAtMs) < 0) return;

  const uint32_t jobId = rawActiveJobId;
  const uint64_t bytes = rawBytesReceived;

  if (rawClientAllocated()) rawClient.stop();

  Log.printf("[RAW] Job #%lu COMPLETE: %llu bytes received and accepted by USB\n",
                (unsigned long)jobId, (unsigned long long)bytes);
  JobLog::record(JobLog::RAW, jobId, 9, "job-completed-successfully",
                 "usb-hand-off-drained", (uint32_t)bytes,
                 rawJobStartedMs ? (uint32_t)(millis() - rawJobStartedMs) : 0,
                 rawClientIp.c_str(), "raw");
  resetRawTransportState();
}

// Push everything the ring still holds to the printer.  Called both during
// normal streaming and while a job is closing, so a FIN or PJL EOJ never
// strands undelivered bytes in the ring (which used to drop the job tail).
bool drainRingToUsb(UsbPrinterBackend *backend) {
  while (ringDataLen > 0) {
    const size_t want = min(static_cast<size_t>(ringDataLen), static_cast<size_t>(RAW_USB_CHUNK));
    const size_t spaceToEnd = USB_BUFFER_SIZE - ringReadPos;
    const size_t want2 = min(want, spaceToEnd);
    String error;
    if (!backend->sendDirect(rawChunk + ringReadPos, want2, error)) {
      Log.printf("[RAW] Job #%lu USB pass-through failed after %llu bytes: %s\n",
                    (unsigned long)rawActiveJobId,
                    (unsigned long long)rawBytesReceived,
                    error.c_str());
      abortRawConnection(backend, error);
      return false;
    }
    rawBytesReceived += (size_t)want2;
    ringReadPos = (ringReadPos + want2) % USB_BUFFER_SIZE;
    ringDataLen -= want2;
    esp_task_wdt_reset();
    yield();
  }
  return true;
}

void handleRawServer(UsbPrinterBackend *backend) {
  if (!rawServerStarted) return;

  if (rawTransportClosing) {
    if (ringDataLen > 0) {
      if (!drainRingToUsb(backend)) return;
      if (ringDataLen > 0) return;  // still draining the job tail; continue next poll
    }
    serviceRawTransportClose(backend);
    return;
  }

  if (!rawClientAllocated()) {
    WiFiClient incoming = rawServer.accept();
    if (incoming.fd() >= 0) {
      if (backend->ippDocumentActive()) {
        // An IPP document is still uploading.  The printer can still be IDLE
        // here, so the state check below would not reject this client; without
        // this guard both sources would share the Bulk OUT pipe.
        Log.println("[RAW] Refusing TCP 9100 connection: IPP document is in progress");
        incoming.stop();
        return;
      }
      if (!backend->online()) {
        Log.printf("[RAW] Printer not ready; rejecting TCP 9100 connection: %s\n",
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
      rawJobStartedMs = rawLastDataMs;
      rawClientIp = incoming.remoteIP().toString();
      pjlDetector.reset();
      Log.printf("[RAW] Job #%lu TCP 9100 client connected; transparent USB pass-through\n",
                    (unsigned long)rawActiveJobId);
    }
    return;
  }

  bool movedData = false;
  static uint32_t consecutiveEmptyReads = 0;

  // Read TCP data and stream directly to USB in chunks
  while (rawClient.available() > 0 && ringDataLen < USB_BUFFER_SIZE) {
    const size_t available = (size_t)rawClient.available();
    const size_t spaceLeft = USB_BUFFER_SIZE - ringDataLen;
    if (spaceLeft == 0) break;  // Buffer full
    const size_t want = min(static_cast<size_t>(available), static_cast<size_t>(RAW_RX_CHUNK));
    const size_t want2 = min(want, spaceLeft);

    // Handle wraparound: split write if it crosses end of buffer
    size_t firstPart = want2;
    if (ringWritePos + want2 > USB_BUFFER_SIZE) {
      firstPart = USB_BUFFER_SIZE - ringWritePos;
    }
    const int got = rawClient.read(rawChunk + ringWritePos, firstPart);
    if (got <= 0) break;

    // Log the job only once real print data arrives: clients and network
    // scanners probe TCP 9100 by opening and closing it with no payload, and
    // those probes must not fill the persisted job ring.
    if (!rawJobLogged) {
      rawJobLogged = true;
      JobLog::record(JobLog::RAW, rawActiveJobId, 3, "job-incoming", "raw-9100",
                     0, 0, rawClientIp.c_str(), "raw");
    }

    // Publish the bytes we just read before looking for PJL EOJ: the EOJ chunk
    // is the last part of the job, so dropping it would truncate the page.
    const uint32_t chunkStart = ringWritePos;
    ringWritePos = (ringWritePos + got) % USB_BUFFER_SIZE;
    ringDataLen += got;
    rawLastDataMs = millis();
    movedData = true;

    // Scan for PJL EOJ sequence in received data. The read above is clamped to
    // firstPart, so the bytes are contiguous in the ring: index them directly
    // instead of paying a modulo per byte.
    bool eojDetected = false;
    for (int i = 0; i < got; ++i) {
      if (pjlDetector.feed(rawChunk[chunkStart + i])) { eojDetected = true; break; }
    }

    if (eojDetected) {
      Log.printf("[RAW] Job #%lu PJL EOJ detected at byte %llu\n",
                    (unsigned long)rawActiveJobId,
                    (unsigned long long)rawBytesReceived);
      pjlDetector.reset();
      // Flush what we received (including the EOJ chunk) before finalising,
      // otherwise the last USB transfers stay queued in the stream ring.
      if (!drainRingToUsb(backend)) return;
      finishRawConnection(backend, "pjl-eoj-detected");
      return;
    }

    esp_task_wdt_reset();
    yield();
  }

  // Drain ring buffer to USB (failure inside aborts the job)
  drainRingToUsb(backend);

  // Track consecutive empty reads for retry logic
  if (!movedData) {
    consecutiveEmptyReads++;
  } else {
    consecutiveEmptyReads = 0;
  }

  // Give the client time to recover from temporary stalls before aborting.
  // Use time-based check: abort only if no data received for RAW_IDLE_TIMEOUT_MS.
  if (rawBytesReceived > 0 &&
      millis() - rawLastDataMs >= RAW_IDLE_TIMEOUT_MS) {
    Log.printf("[RAW] Job #%lu TCP client stalled after %llu bytes; no data for %lu ms\n",
                (unsigned long)rawActiveJobId,
                (unsigned long long)rawBytesReceived,
                millis() - rawLastDataMs);
    abortRawConnection(backend, "TCP client stalled before sending job FIN");
    return;
  }

  // No data received at all - use probe timeout
  if (rawBytesReceived == 0 &&
      millis() - rawLastDataMs >= RAW_PROBE_IDLE_TIMEOUT_MS) {
    Log.printf("[RAW] TCP 9100 connection #%lu idle with no print data; closing\n",
                (unsigned long)rawActiveJobId);
    if (rawClientAllocated()) rawClient.stop();
    resetRawTransportState();
    return;
  }

  const bool pending = rawClient.available() > 0;
  int peerError = 0;
  const RawPeerState peer = rawPeerState(peerError);

  if (peer == RawPeerState::CLEAN_FIN && !pending && ringDataLen == 0) {
    finishRawConnection(backend, "clean-peer-FIN-and-buffer-drained");
    return;
  }

  if (peer == RawPeerState::ERROR && !pending) {
    // Some Android print applications reset the socket after their last write
    // instead of performing a graceful FIN.  At this point the receive buffer
    // is empty and every byte counted in rawBytesReceived has already passed
    // through sendDirect(), so the reset is only a transport close signal.
    // Treating it as an aborted job loses otherwise-complete Android jobs.
    if (rawBytesReceived > 0 && ringDataLen == 0 &&
        (peerError == ECONNRESET || peerError == ECONNABORTED || peerError == EPIPE)) {
      Log.printf("[RAW] Job #%lu peer reset after buffer drained; errno=%d; treating as input complete\n",
                    (unsigned long)rawActiveJobId, peerError);
      finishRawConnection(backend, "android-peer-reset-after-buffer-drained");
    } else {
      abortRawConnection(backend,
                         String("TCP peer ended without clean FIN; errno=") + String(peerError));
    }
    return;
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
    Log.println("[RAW] JetDirect/AppSocket listening on TCP 9100");
  }
}
} // namespace

bool UsbPrinterBackend::rawClientConnected() const {
  return rawClientAllocated() || rawTransportClosing;
}

void UsbPrinterBackend::restartRawListener() {
  // Rebind only the listening socket; an accepted client survives this and a
  // job in progress must not be aborted by a Wi-Fi transition.
  if (rawClientAllocated() || rawTransportClosing) return;
  if (rawServerStarted) {
    rawServer.end();
    rawServerStarted = false;
  }
  startRawServerIfNeeded();
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
  
  // Allocate multi-chunk PSRAM buffer for USB transfers
  if (!rawChunk) {
    rawChunk = (uint8_t *)heap_caps_malloc(USB_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
    if (!rawChunk) {
      Log.println("[USB] PSRAM alloc failed for rawChunk, using heap");
      rawChunk = (uint8_t *)malloc(USB_BUFFER_SIZE);
    }
    if (rawChunk) {
      Log.printf("[USB] Allocated %u byte USB buffer in PSRAM\n", USB_BUFFER_SIZE);
    } else {
      Log.println("[USB] Failed to allocate USB buffer!");
    }
  }
  if (!stream_.begin(host_)) {
    Log.println("[USB] Dedicated stream writer unavailable; USB writes stay on the loop task");
  }
  return true;
}

void UsbPrinterBackend::completeDrainIfReady() {
  if (!drainPending_ || state_ != PRINTING) return;

  const unsigned long now = millis();
  const unsigned long age = now - drainStartedMs_;
  const bool statusValid = usbStatusValid();
  const bool freshStatus = statusValid && host_.portStatus().updatedAt > drainStatusAtStart_;

  esp_task_wdt_reset();

  // Paper-empty and the standard Printer Class error bit are actionable failures.
  if (statusValid && usbPaperEmpty()) {
    drainPending_ = false;
    state_ = OFFLINE;
    reason_ = "usb-printer-reports-paper-empty";
    jobsFailed_++;
    lastError_ = reason_;
    StatusLed::set(StatusLed::WAITING_FOR_PRINTER);
    Log.printf("[PRINT][FINALIZE] stopped: paper empty after %llu USB-confirmed bytes\n",
                  (unsigned long long)jobBytes_);
    jobBytes_ = 0;
    return;
  }
  if (statusValid && usbStatusError()) {
    drainPending_ = false;
    state_ = ERROR;
    reason_ = "usb-printer-reports-error-after-job";
    jobsFailed_++;
    lastError_ = reason_;
    StatusLed::set(StatusLed::ERROR);
    Log.printf("[PRINT][FINALIZE] failed: printer error after %llu USB-confirmed bytes\n",
                  (unsigned long long)jobBytes_);
    jobBytes_ = 0;
    return;
  }

  if ((int32_t)(now - drainNextLogMs_) >= 0) {
    drainNextLogMs_ = now + POST_JOB_LOG_INTERVAL_MS;
    Log.printf("[PRINT][FINALIZE] waiting age=%lu ms status=%s fresh=%s value=0x%02X selected=%s\n",
                  age,
                  statusValid ? "valid" : "unavailable",
                  freshStatus ? "yes" : "no",
                  statusValid ? usbPortStatus() : 0,
                  statusValid ? (usbStatusSelected() ? "yes" : "no") : "unknown");
  }

  // Do not call a temporary de-selected state a failed job. HP devices can move the
  // tray/paper while the network document has already finished. Keep the job in
  // processing until the printer is selected again or the conservative timeout expires.
  if (statusValid && !usbStatusSelected()) {
    reason_ = "printer-processing-or-not-selected";
    if (age < POST_JOB_STATUS_WAIT_MAX_MS) return;

    drainPending_ = false;
    state_ = OFFLINE;
    reason_ = "usb-printer-reports-not-selected-after-timeout";
    StatusLed::set(StatusLed::WAITING_FOR_PRINTER);
    Log.printf("[PRINT][FINALIZE] stopped: printer did not return selected within %lu ms\n",
                  (unsigned long)POST_JOB_STATUS_WAIT_MAX_MS);
    jobBytes_ = 0;
    return;
  }

  if (age < POST_JOB_MIN_GUARD_MS) return;

  // With valid status, insist on a sample obtained after the final document byte.
  // With no status support, finish after the conservative minimum guard rather than
  // leaving the queue stuck forever.
  if (statusValid && !freshStatus && age < POST_JOB_STATUS_WAIT_MAX_MS) return;

  drainPending_ = false;
  state_ = IDLE;
  reason_ = freshStatus ? "usb-printer-ready-after-post-job-guard"
                        : "printer-ready-after-post-job-guard";
  if (sourceAborted_) {
    jobsFailed_++;
    lastError_ = "source-ended-early";
  } else {
    jobsCompleted_++;
  }
  sourceAborted_ = false;
  if (age > maxDrainMs_) maxDrainMs_ = age;
  StatusLed::set(StatusLed::PRINTER_READY);
  Log.printf("[PRINT][FINALIZE] transport complete: %llu bytes; waited=%lu ms; status=%s fresh=%s value=0x%02X\n",
                (unsigned long long)jobBytes_, age,
                statusValid ? "valid" : "unavailable",
                freshStatus ? "yes" : "no",
                statusValid ? usbPortStatus() : 0);
  jobBytes_ = 0;
}

void UsbPrinterBackend::poll() {
  startRawServerIfNeeded();

  // A detach while a job is active must fail it now.  Otherwise the PRINTING
  // early-return below keeps completeDrainIfReady() waiting out the post-job
  // timeout against a printer that no longer exists, and new IPP jobs keep
  // being refused as "still printing".
  if (state_ == PRINTING && !host_.device().attached) {
    abortRawJob("USB printer disconnected");
    StatusLed::update();
    return;
  }

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
    jobStartedMs_ = millis();
    nextHealthLogAt_ = HEALTH_LOG_INTERVAL_BYTES;
    drainPending_ = false;
    sourceAborted_ = false;
    stream_.reset();
  }
  state_ = PRINTING;
  for (int retry = 0; retry < 3; retry++) {
    bool queued;
    if (stream_.ready()) {
      queued = stream_.push(data, length, USB_STREAM_PUSH_TIMEOUT_MS);
      if (!queued && !stream_.failed(error)) error = "USB stream rejected print data";
    } else {
      queued = sendUsbChunk(host_, data, length, error);
    }
    if (queued) {
      jobBytes_ += length;
      if (jobBytes_ >= nextHealthLogAt_) {
        logTransportHealth(jobBytes_, (uint32_t)(millis() - jobStartedMs_));
        while (nextHealthLogAt_ <= jobBytes_) nextHealthLogAt_ += HEALTH_LOG_INTERVAL_BYTES;
      }
      reason_ = "raw-job-in-progress";
      return true;
    }
    delay(100);
  }
  abortRawJob(error);
  return false;
}

bool UsbPrinterBackend::finishRawJobStep() {
  if (state_ != PRINTING) return true;
  if (drainPending_) return true;

  if (stream_.ready()) {
    String failure;
    if (stream_.failed(failure)) {
      abortRawJob(failure);
      return true;
    }
    if (!stream_.idle()) return false;
  }
  // The writer task may be idle while a pipelined transfer is still completing.
  if (!host_.bulkOutIdle()) return false;

  // Everything the document produced has been accepted by the USB host driver.
  // finishRawJob() is now cheap: its flush/wait calls return immediately, and
  // it arms the existing poll-driven post-job guard.
  finishRawJob();
  return true;
}

void UsbPrinterBackend::finishRawJob() {
  if (state_ != PRINTING || drainPending_) return;
  String flushError;
  const bool flushed = stream_.ready()
                           ? stream_.flush(RAW_USB_TIMEOUT_MS, flushError)
                           : host_.waitForBulkOut(RAW_USB_TIMEOUT_MS, flushError);
  if (!flushed) {
    Log.printf("[PRINT][FINALIZE] bulk-out drain failed after %u job bytes: %s\n",
               (unsigned)jobBytes_, flushError.c_str());
    abortRawJob(flushError);
    return;
  }
  // Terminate the bulk stream with a zero-length packet.  ESP-IDF does not
  // send one automatically when the final transfer is an exact multiple of
  // the endpoint MPS (64 bytes), so without this the printer's USB stack can
  // hold the last full packet and never flush the tail of the document: the
  // symptom is a partial page followed by an error/eject.  A ZLP failure is
  // non-fatal because the data has already been handed off.
  {
    size_t zlpAccepted = 0;
    String zlpError;
    if (!host_.bulkWrite(nullptr, 0, zlpAccepted, RAW_USB_TIMEOUT_MS, zlpError)) {
      Log.printf("[PRINT][FINALIZE] trailing ZLP failed: %s (non-fatal)\n",
                 zlpError.c_str());
    } else {
      Log.println("[PRINT][FINALIZE] trailing ZLP sent");
    }
  }
  // Flush the printer's buffers and clear any error state before the next job.
  // Non-fatal: a printer that doesn't support SOFT_RESET just ignores it.
  {
    String softResetError;
    if (host_.softReset(RAW_USB_TIMEOUT_MS, softResetError)) {
      Log.println("[PRINT][FINALIZE] soft reset OK");
    } else {
      Log.printf("[PRINT][FINALIZE] soft reset skipped/failed: %s (non-fatal)\n",
                 softResetError.c_str());
    }
  }
  const uint32_t handoffMs = (uint32_t)(millis() - jobStartedMs_);
  const double kb = jobBytes_ / 1024.0;
  Log.printf("[PRINT][RATE] USB hand-off: %llu bytes in %lu ms (%.1f KB/s)\n",
             (unsigned long long)jobBytes_, (unsigned long)handoffMs,
             handoffMs ? (kb * 1000.0 / handoffMs) : 0.0);
  drainPending_ = true;
  drainStartedMs_ = millis();
  drainNextLogMs_ = drainStartedMs_;
  drainStatusAtStart_ = usbStatusValid() ? host_.portStatus().updatedAt : 0;
  reason_ = "raw-job-draining";
  Log.printf("[PRINT][FINALIZE] last USB document byte accepted; minimum-guard=%lu ms max-status-wait=%lu ms status-at-start=%lu\n",
                (unsigned long)POST_JOB_MIN_GUARD_MS,
                (unsigned long)POST_JOB_STATUS_WAIT_MAX_MS,
                (unsigned long)drainStatusAtStart_);
}

void UsbPrinterBackend::abortRawJob(const String &reason) {
  if (state_ != ERROR) {
    jobsFailed_++;
    lastError_ = reason.length() ? reason : "raw-job-aborted";
  }
  drainPending_ = false;
  jobBytes_ = 0;
  state_ = ERROR;
  reason_ = reason.length() ? reason : "raw-job-aborted";
  // Drop anything still queued for the failed job so the next document cannot
  // inherit its tail.  In-flight transfers are allowed to finish: cancelling
  // them would require halting the endpoint, which turns a cancelled job into
  // a USB recovery cycle.  reset() clears everything not yet handed off.
  stream_.reset();
  StatusLed::set(StatusLed::ERROR);
}

void UsbPrinterBackend::endRawJobOnSourceAbort(const String &reason) {
  if (state_ != PRINTING) return;
  // The USB link is healthy; only the document source ended early.  Finish the
  // hand-off and terminate the stream the same way a normal job ends (flush,
  // ZLP, SOFT_RESET, post-job drain).  This lets the printer finish or eject
  // the partial page cleanly instead of being abandoned mid-raster, which is
  // what made the printer retry / eject extra sheets.
  sourceAborted_ = true;
  Log.printf("[PRINT][ABORT] source ended early (%s) after %llu USB-confirmed bytes; terminating stream cleanly\n",
             reason.c_str(), (unsigned long long)jobBytes_);
  finishRawJob();
}