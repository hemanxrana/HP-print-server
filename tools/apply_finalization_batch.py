from pathlib import Path
import re


def rep(path, old, new):
    p = Path(path)
    s = p.read_text()
    if old not in s:
        raise RuntimeError(f"marker not found in {path}: {old[:160]!r}")
    p.write_text(s.replace(old, new, 1))


def rx(path, pattern, repl):
    p = Path(path)
    s = p.read_text()
    out, n = re.subn(pattern, lambda m: repl, s, count=1, flags=re.S)
    if n != 1:
        raise RuntimeError(f"regex failed in {path}, matches={n}: {pattern[:160]!r}")
    p.write_text(out)


# Make post-job state conservative. GET_PORT_STATUS has no explicit mechanical-busy bit,
# so do not treat a transient de-selected state during tray/page motion as an immediate failure.
rep(
    "usb_printer_backend.cpp",
    "constexpr uint32_t RAW_JOB_DRAIN_MS = 8000;\nconstexpr uint32_t POST_JOB_STATUS_WAIT_MAX_MS = 15000;\nconstexpr uint64_t HEALTH_LOG_INTERVAL_BYTES = 512ULL * 1024ULL;",
    "constexpr uint32_t POST_JOB_MIN_GUARD_MS = 15000;\n"
    "constexpr uint32_t POST_JOB_STATUS_WAIT_MAX_MS = 30000;\n"
    "constexpr uint32_t POST_JOB_LOG_INTERVAL_MS = 2000;\n"
    "constexpr uint64_t HEALTH_LOG_INTERVAL_BYTES = 512ULL * 1024ULL;"
)

rx(
    "usb_printer_backend.cpp",
    r"void UsbPrinterBackend::completeDrainIfReady\(\) \{.*?\n\}\n\nvoid UsbPrinterBackend::poll\(\)",
    '''void UsbPrinterBackend::completeDrainIfReady() {
  if (!drainPending_ || state_ != PRINTING) return;

  const unsigned long now = millis();
  const unsigned long age = now - drainStartedMs_;
  const bool statusValid = usbStatusValid();
  const bool freshStatus = statusValid && host_.portStatus().updatedAt > drainStatusAtStart_;

  // Paper-empty and the standard Printer Class error bit are actionable failures.
  if (statusValid && usbPaperEmpty()) {
    drainPending_ = false;
    state_ = OFFLINE;
    reason_ = "usb-printer-reports-paper-empty";
    StatusLed::set(StatusLed::WAITING_FOR_PRINTER);
    Serial.printf("[PRINT][FINALIZE] stopped: paper empty after %llu USB-confirmed bytes\\n",
                  (unsigned long long)jobBytes_);
    jobBytes_ = 0;
    return;
  }
  if (statusValid && usbStatusError()) {
    drainPending_ = false;
    state_ = ERROR;
    reason_ = "usb-printer-reports-error-after-job";
    StatusLed::set(StatusLed::ERROR);
    Serial.printf("[PRINT][FINALIZE] failed: printer error after %llu USB-confirmed bytes\\n",
                  (unsigned long long)jobBytes_);
    jobBytes_ = 0;
    return;
  }

  if ((int32_t)(now - drainNextLogMs_) >= 0) {
    drainNextLogMs_ = now + POST_JOB_LOG_INTERVAL_MS;
    Serial.printf("[PRINT][FINALIZE] waiting age=%lu ms status=%s fresh=%s value=0x%02X selected=%s\\n",
                  age,
                  statusValid ? "valid" : "unavailable",
                  freshStatus ? "yes" : "no",
                  statusValid ? usbPortStatus() : 0,
                  statusValid ? (usbStatusSelected() ? "yes" : "no") : "unknown");
  }

  // Do not call a temporary de-selected state a failed job. HP devices can move the
  // tray/paper while the network document has already finished. Keep the IPP job in
  // processing until the printer is selected again or the conservative timeout expires.
  if (statusValid && !usbStatusSelected()) {
    reason_ = "printer-processing-or-not-selected";
    if (age < POST_JOB_STATUS_WAIT_MAX_MS) return;

    drainPending_ = false;
    state_ = OFFLINE;
    reason_ = "usb-printer-reports-not-selected-after-timeout";
    StatusLed::set(StatusLed::WAITING_FOR_PRINTER);
    Serial.printf("[PRINT][FINALIZE] stopped: printer did not return selected within %lu ms\\n",
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
  StatusLed::set(StatusLed::PRINTER_READY);
  Serial.printf("[PRINT][FINALIZE] transport complete: %llu bytes; waited=%lu ms; status=%s fresh=%s value=0x%02X\\n",
                (unsigned long long)jobBytes_, age,
                statusValid ? "valid" : "unavailable",
                freshStatus ? "yes" : "no",
                statusValid ? usbPortStatus() : 0);
  jobBytes_ = 0;
}

void UsbPrinterBackend::poll()'''
)

rep(
    "usb_printer_backend.cpp",
    '''void UsbPrinterBackend::finishRawJob() {
  if (state_ != PRINTING || drainPending_) return;
  drainPending_ = true;
  drainStartedMs_ = millis();
  drainUntilMs_ = drainStartedMs_ + RAW_JOB_DRAIN_MS;
  drainStatusAtStart_ = usbStatusValid() ? host_.portStatus().updatedAt : 0;
  reason_ = "raw-job-draining";
  Serial.printf("[PRINT][FINALIZE] last USB document byte accepted; guard=%lu ms status-at-start=%lu\\n",
                (unsigned long)RAW_JOB_DRAIN_MS, (unsigned long)drainStatusAtStart_);
}''',
    '''void UsbPrinterBackend::finishRawJob() {
  if (state_ != PRINTING || drainPending_) return;
  drainPending_ = true;
  drainStartedMs_ = millis();
  drainUntilMs_ = drainStartedMs_ + POST_JOB_MIN_GUARD_MS;
  drainNextLogMs_ = drainStartedMs_;
  drainStatusAtStart_ = usbStatusValid() ? host_.portStatus().updatedAt : 0;
  reason_ = "raw-job-draining";
  Serial.printf("[PRINT][FINALIZE] last USB document byte accepted; minimum-guard=%lu ms max-status-wait=%lu ms status-at-start=%lu\\n",
                (unsigned long)POST_JOB_MIN_GUARD_MS,
                (unsigned long)POST_JOB_STATUS_WAIT_MAX_MS,
                (unsigned long)drainStatusAtStart_);
}'''
)

rep(
    "usb_printer_backend.h",
    "  unsigned long drainStartedMs_ = 0;\n  uint32_t drainStatusAtStart_ = 0;",
    "  unsigned long drainStartedMs_ = 0;\n"
    "  unsigned long drainNextLogMs_ = 0;\n"
    "  uint32_t drainStatusAtStart_ = 0;"
)

# IPP: a real backend OFFLINE state should be exposed as stopped, not remain forever at processing.
rx(
    "ipp_pcl3_service.cpp",
    r"void IppPcl3Service::refreshJobState\(\)\{.*?\n\}\n\nvoid IppPcl3Service::begin\(\)",
    '''void IppPcl3Service::refreshJobState(){
  if(lastJobId_==0 || lastJobState_!=5) return;
  const auto state = printer_.state();
  if(state==UsbPrinterBackend::IDLE){
    lastJobState_=9;
    lastJobReason_="job-completed-successfully";
    Serial.printf("[IPP] Job %lu state -> completed\\n",(unsigned long)lastJobId_);
  } else if(state==UsbPrinterBackend::OFFLINE){
    lastJobState_=6;
    lastJobReason_="printer-stopped";
    Serial.printf("[IPP] Job %lu state -> stopped: %s\\n",
                  (unsigned long)lastJobId_,printer_.statusReason().c_str());
  } else if(state==UsbPrinterBackend::ERROR){
    lastJobState_=8;
    lastJobReason_="aborted-by-system";
    Serial.printf("[IPP] Job %lu state -> aborted: %s\\n",
                  (unsigned long)lastJobId_,printer_.statusReason().c_str());
  }
}

void IppPcl3Service::begin()'''
)

# Arduino-ESP32 3.3.x renamed NetworkServer::available() to accept().
rep("ipp_pcl3_service.cpp", "  WiFiClient client=ippServer.available();", "  WiFiClient client=ippServer.accept();")
rep("usb_printer_backend.cpp", "    WiFiClient incoming = rawServer.available();", "    WiFiClient incoming = rawServer.accept();")

# Correct printf types on ESP32-S3, where uint32_t is unsigned long in this toolchain.
s = Path("usb_printer_backend.cpp").read_text()
s = s.replace("[RAW] Job #%u", "[RAW] Job #%lu")
s = s.replace("[RAW] TCP 9100 connection #%u", "[RAW] TCP 9100 connection #%lu")
s = s.replace("rawActiveJobId, reason);", "(unsigned long)rawActiveJobId, reason);")
s = s.replace("rawActiveJobId, (unsigned long)RAW_CLOSE_GUARD_MS", "(unsigned long)rawActiveJobId, (unsigned long)RAW_CLOSE_GUARD_MS")
s = s.replace("rawActiveJobId);", "(unsigned long)rawActiveJobId);")
s = s.replace("jobId, (unsigned long long)bytes", "(unsigned long)jobId, (unsigned long long)bytes")
s = s.replace("rawActiveJobId, reason,\n", "(unsigned long)rawActiveJobId, reason,\n")
s = s.replace("rawActiveJobId,\n                    (unsigned long long)rawBytesReceived", "(unsigned long)rawActiveJobId,\n                    (unsigned long long)rawBytesReceived")
Path("usb_printer_backend.cpp").write_text(s)

print("Applied finalization/status batch")
# trigger after workflow creation
