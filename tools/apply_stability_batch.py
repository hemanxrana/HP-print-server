from pathlib import Path
import re


def replace_once(path, old, new):
    p = Path(path)
    s = p.read_text()
    if old not in s:
        raise RuntimeError(f"marker not found in {path}: {old[:100]!r}")
    p.write_text(s.replace(old, new, 1))


def regex_once(path, pattern, repl):
    p = Path(path)
    s = p.read_text()
    out, n = re.subn(pattern, repl, s, count=1, flags=re.S)
    if n != 1:
        raise RuntimeError(f"regex marker not found/ambiguous in {path}: {pattern[:100]!r}, matches={n}")
    p.write_text(out)


# 1) Reuse one Bulk OUT transfer + semaphore instead of allocating/freeing them
# for every 1 KiB packet. This is shared by RAW 9100 and IPP.
replace_once(
    "usb_host_manager.cpp",
    "static constexpr uint32_t STATUS_TRANSFER_TIMEOUT_MS = 1000;\n\nstruct TransferWait { SemaphoreHandle_t done = nullptr; };",
    "static constexpr uint32_t STATUS_TRANSFER_TIMEOUT_MS = 1000;\n"
    "static constexpr size_t BULK_OUT_BUFFER_SIZE = 1024;\n\n"
    "struct TransferWait { SemaphoreHandle_t done = nullptr; };"
)

replace_once(
    "usb_host_manager.cpp",
    "  volatile bool statusPending = false;\n  volatile usb_transfer_t *statusTransfer = nullptr;\n\n  TaskHandle_t clientTask = nullptr;",
    "  volatile bool statusPending = false;\n  volatile usb_transfer_t *statusTransfer = nullptr;\n\n"
    "  usb_transfer_t *bulkOutTransfer = nullptr;\n"
    "  TransferWait bulkOutWait;\n"
    "  volatile bool bulkOutInFlight = false;\n\n"
    "  TaskHandle_t clientTask = nullptr;"
)

replace_once(
    "usb_host_manager.cpp",
    "static void resetDevice() {\n",
    "static void freeBulkOutResources() {\n"
    "  if (g.bulkOutTransfer && !g.bulkOutInFlight) {\n"
    "    usb_host_transfer_free(g.bulkOutTransfer);\n"
    "    g.bulkOutTransfer = nullptr;\n"
    "  }\n"
    "  if (g.bulkOutWait.done && !g.bulkOutInFlight) {\n"
    "    vSemaphoreDelete(g.bulkOutWait.done);\n"
    "    g.bulkOutWait.done = nullptr;\n"
    "  }\n"
    "}\n\n"
    "static bool ensureBulkOutResources(String &error) {\n"
    "  if (!g.bulkOutWait.done) {\n"
    "    g.bulkOutWait.done = xSemaphoreCreateBinary();\n"
    "    if (!g.bulkOutWait.done) {\n"
    "      error = \"unable to allocate persistent USB completion semaphore\";\n"
    "      return false;\n"
    "    }\n"
    "  }\n"
    "  if (!g.bulkOutTransfer) {\n"
    "    const esp_err_t e = usb_host_transfer_alloc(BULK_OUT_BUFFER_SIZE, 0, &g.bulkOutTransfer);\n"
    "    if (e != ESP_OK || !g.bulkOutTransfer) {\n"
    "      error = String(\"persistent usb_host_transfer_alloc failed: \") + esp_err_to_name(e);\n"
    "      vSemaphoreDelete(g.bulkOutWait.done);\n"
    "      g.bulkOutWait.done = nullptr;\n"
    "      return false;\n"
    "    }\n"
    "    Serial.printf(\"[USB] Persistent Bulk OUT transfer allocated once (%u bytes)\\n\",\n"
    "                  (unsigned)BULK_OUT_BUFFER_SIZE);\n"
    "  }\n"
    "  return true;\n"
    "}\n\n"
    "static void resetDevice() {\n"
)

replace_once(
    "usb_host_manager.cpp",
    "  g.statusTransfer = nullptr;\n}\n\nstatic void releaseInterfaces()",
    "  g.statusTransfer = nullptr;\n  freeBulkOutResources();\n}\n\nstatic void releaseInterfaces()"
)

replace_once(
    "usb_host_manager.cpp",
    "static void bulkTransferCallback(usb_transfer_t *t) {\n  if (!t) return;\n  TransferWait *w = static_cast<TransferWait *>(t->context);\n  if (w && w->done) xSemaphoreGive(w->done);\n}",
    "static void bulkTransferCallback(usb_transfer_t *t) {\n"
    "  if (!t) return;\n"
    "  if (t == g.bulkOutTransfer) g.bulkOutInFlight = false;\n"
    "  TransferWait *w = static_cast<TransferWait *>(t->context);\n"
    "  if (w && w->done) xSemaphoreGive(w->done);\n"
    "}"
)

regex_once(
    "usb_host_manager.cpp",
    r"bool UsbHostManager::bulkWrite\(const uint8_t \*data, size_t length, size_t &accepted,\n                               uint32_t timeoutMs, String &error\) \{.*?\n\}\n\nvoid UsbHostManager::onPortStatusTransfer",
    '''bool UsbHostManager::bulkWrite(const uint8_t *data, size_t length, size_t &accepted,
                               uint32_t timeoutMs, String &error) {
  accepted = 0;
  if (!data || !length) {
    error = "empty USB transfer";
    return false;
  }
  if (!g.deviceOpen || !g.printInterfaceClaimed || !device_.printer.usableForRawPrint()) {
    error = "USB printer interface is not claimed/ready";
    return false;
  }
  if (length > BULK_OUT_BUFFER_SIZE) {
    error = String("USB transfer chunk exceeds persistent buffer: ") + String((unsigned)length) +
            "/" + String((unsigned)BULK_OUT_BUFFER_SIZE);
    return false;
  }
  if (g.bulkOutInFlight) {
    error = "previous USB Bulk OUT transfer is still in flight";
    return false;
  }
  if (!ensureBulkOutResources(error)) return false;

  // Drain any stale completion token before reusing the transfer.
  while (xSemaphoreTake(g.bulkOutWait.done, 0) == pdTRUE) {}

  usb_transfer_t *t = g.bulkOutTransfer;
  memcpy(t->data_buffer, data, length);
  t->num_bytes = length;
  t->device_handle = g.device;
  t->bEndpointAddress = device_.printer.bulkOut.address;
  t->callback = bulkTransferCallback;
  t->context = &g.bulkOutWait;
  t->timeout_ms = timeoutMs;

  g.bulkOutInFlight = true;
  const esp_err_t e = usb_host_transfer_submit(t);
  if (e != ESP_OK) {
    g.bulkOutInFlight = false;
    error = String("usb_host_transfer_submit failed: ") + esp_err_to_name(e);
    return false;
  }

  const TickType_t waitTicks = timeoutMs ? pdMS_TO_TICKS(timeoutMs + 250) : portMAX_DELAY;
  if (xSemaphoreTake(g.bulkOutWait.done, waitTicks) != pdTRUE) {
    Serial.println("[USB] Bulk OUT completion wait exceeded transfer timeout; recovering endpoint");
    usb_host_endpoint_halt(t->device_handle, t->bEndpointAddress);
    usb_host_endpoint_flush(t->device_handle, t->bEndpointAddress);
    usb_host_endpoint_clear(t->device_handle, t->bEndpointAddress);
    if (xSemaphoreTake(g.bulkOutWait.done, pdMS_TO_TICKS(1500)) != pdTRUE) {
      error = "USB Bulk OUT callback did not complete after endpoint recovery";
      return false;
    }
  }

  accepted = static_cast<size_t>(t->actual_num_bytes);
  const usb_transfer_status_t status = t->status;
  if (status != USB_TRANSFER_STATUS_COMPLETED || accepted != length) {
    error = String("USB bulk transfer failed status=") + String((int)status) +
            " accepted=" + String((unsigned)accepted) + "/" + String((unsigned)length);
    return false;
  }
  return true;
}

void UsbHostManager::onPortStatusTransfer'''
)

# 2) Health telemetry and a less optimistic post-job completion state.
replace_once(
    "usb_printer_backend.cpp",
    "#include <lwip/sockets.h>\n",
    "#include <lwip/sockets.h>\n#include \"esp_heap_caps.h\"\n#include \"freertos/FreeRTOS.h\"\n#include \"freertos/task.h\"\n"
)
replace_once(
    "usb_printer_backend.cpp",
    "constexpr uint32_t RAW_JOB_DRAIN_MS = 3000;",
    "constexpr uint32_t RAW_JOB_DRAIN_MS = 8000;\n"
    "constexpr uint32_t POST_JOB_STATUS_WAIT_MAX_MS = 15000;\n"
    "constexpr uint64_t HEALTH_LOG_INTERVAL_BYTES = 512ULL * 1024ULL;"
)

replace_once(
    "usb_printer_backend.cpp",
    "const char *statusReasonText(const UsbPrinterBackend &backend) {",
    "void logTransportHealth(uint64_t bytes) {\n"
    "  const uint32_t freeHeap = ESP.getFreeHeap();\n"
    "  const uint32_t minHeap = ESP.getMinFreeHeap();\n"
    "  const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);\n"
    "  const UBaseType_t stackWords = uxTaskGetStackHighWaterMark(nullptr);\n"
    "  Serial.printf(\"[PRINT][HEALTH] bytes=%llu free-heap=%lu min-free-heap=%lu largest-block=%u loop-stack-watermark=%u words\\n\",\n"
    "                (unsigned long long)bytes, (unsigned long)freeHeap, (unsigned long)minHeap,\n"
    "                (unsigned)largest, (unsigned)stackWords);\n"
    "}\n\n"
    "const char *statusReasonText(const UsbPrinterBackend &backend) {"
)

replace_once(
    "usb_printer_backend.cpp",
    "  jobBytes_ = 0;\n  drainPending_ = false;\n  return true;",
    "  jobBytes_ = 0;\n  nextHealthLogAt_ = HEALTH_LOG_INTERVAL_BYTES;\n  drainPending_ = false;\n  drainStartedMs_ = 0;\n  drainStatusAtStart_ = 0;\n  return true;"
)

regex_once(
    "usb_printer_backend.cpp",
    r"void UsbPrinterBackend::completeDrainIfReady\(\) \{.*?\n\}\n\nvoid UsbPrinterBackend::poll\(\)",
    '''void UsbPrinterBackend::completeDrainIfReady() {
  if (!drainPending_ || state_ != PRINTING) return;

  // Surface a real Printer Class error immediately instead of waiting out the guard.
  if (usbStatusValid() && usbStatusError()) {
    drainPending_ = false;
    state_ = ERROR;
    reason_ = "usb-printer-reports-error-after-job";
    StatusLed::set(StatusLed::ERROR);
    Serial.printf("[PRINT][FINALIZE] printer reported error after %llu USB-confirmed bytes\\n",
                  (unsigned long long)jobBytes_);
    jobBytes_ = 0;
    return;
  }
  if (usbStatusValid() && (usbPaperEmpty() || !usbStatusSelected())) {
    drainPending_ = false;
    state_ = OFFLINE;
    reason_ = statusReasonText(*this);
    StatusLed::set(StatusLed::WAITING_FOR_PRINTER);
    Serial.printf("[PRINT][FINALIZE] printer not ready after job: %s\\n", reason_.c_str());
    jobBytes_ = 0;
    return;
  }

  const unsigned long now = millis();
  if ((int32_t)(now - drainUntilMs_) < 0) return;

  // GET_PORT_STATUS has no explicit "mechanically finished" bit. Require at least
  // one fresh status sample taken after the final document byte before declaring
  // the transport complete. If status is unavailable, use the conservative guard.
  const bool statusUnavailable = !usbStatusValid();
  const bool freshStatus = usbStatusValid() && host_.portStatus().updatedAt > drainStatusAtStart_;
  if (!statusUnavailable && !freshStatus && now - drainStartedMs_ < POST_JOB_STATUS_WAIT_MAX_MS) return;

  drainPending_ = false;
  state_ = IDLE;
  reason_ = freshStatus ? "usb-printer-ready-after-fresh-status" : "printer-ready-after-post-job-guard";
  StatusLed::set(StatusLed::PRINTER_READY);

  Serial.printf("[PRINT][FINALIZE] transport complete: %llu bytes; waited=%lu ms; status=%s fresh=%s value=0x%02X\\n",
                (unsigned long long)jobBytes_,
                (unsigned long)(now - drainStartedMs_),
                usbStatusValid() ? "valid" : "unavailable",
                freshStatus ? "yes" : "no",
                usbStatusValid() ? usbPortStatus() : 0);
  jobBytes_ = 0;
}

void UsbPrinterBackend::poll()'''
)

replace_once(
    "usb_printer_backend.cpp",
    "  if (state_ == IDLE) {\n    jobBytes_ = 0;\n    drainPending_ = false;\n  }",
    "  if (state_ == IDLE) {\n    jobBytes_ = 0;\n    nextHealthLogAt_ = HEALTH_LOG_INTERVAL_BYTES;\n    drainPending_ = false;\n  }"
)

replace_once(
    "usb_printer_backend.cpp",
    "  jobBytes_ += length;\n  reason_ = \"raw-job-in-progress\";",
    "  jobBytes_ += length;\n"
    "  if (jobBytes_ >= nextHealthLogAt_) {\n"
    "    logTransportHealth(jobBytes_);\n"
    "    while (nextHealthLogAt_ <= jobBytes_) nextHealthLogAt_ += HEALTH_LOG_INTERVAL_BYTES;\n"
    "  }\n"
    "  reason_ = \"raw-job-in-progress\";"
)

replace_once(
    "usb_printer_backend.cpp",
    "void UsbPrinterBackend::finishRawJob() {\n  if (state_ != PRINTING || drainPending_) return;\n  drainPending_ = true;\n  drainUntilMs_ = millis() + RAW_JOB_DRAIN_MS;\n  reason_ = \"raw-job-draining\";\n}",
    "void UsbPrinterBackend::finishRawJob() {\n"
    "  if (state_ != PRINTING || drainPending_) return;\n"
    "  drainPending_ = true;\n"
    "  drainStartedMs_ = millis();\n"
    "  drainUntilMs_ = drainStartedMs_ + RAW_JOB_DRAIN_MS;\n"
    "  drainStatusAtStart_ = usbStatusValid() ? host_.portStatus().updatedAt : 0;\n"
    "  reason_ = \"raw-job-draining\";\n"
    "  Serial.printf(\"[PRINT][FINALIZE] last USB document byte accepted; guard=%lu ms status-at-start=%lu\\n\",\n"
    "                (unsigned long)RAW_JOB_DRAIN_MS, (unsigned long)drainStatusAtStart_);\n"
    "}"
)

replace_once(
    "usb_printer_backend.h",
    "  uint64_t jobBytes_ = 0;\n  bool drainPending_ = false;\n  unsigned long drainUntilMs_ = 0;",
    "  uint64_t jobBytes_ = 0;\n"
    "  uint64_t nextHealthLogAt_ = 512ULL * 1024ULL;\n"
    "  bool drainPending_ = false;\n"
    "  unsigned long drainUntilMs_ = 0;\n"
    "  unsigned long drainStartedMs_ = 0;\n"
    "  uint32_t drainStatusAtStart_ = 0;"
)

# 3) Boot/reset diagnostics. This lets one hardware run distinguish watchdog,
# panic/software reset, brownout/power, or a normal reset without another build.
Path("system_diagnostics.h").write_text('''#pragma once\n\nnamespace SystemDiagnostics {\nvoid logBoot();\n}\n''')

Path("system_diagnostics.cpp").write_text(r'''#include "system_diagnostics.h"
#include <Arduino.h>
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
const char *resetReasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_EXT: return "external-reset";
    case ESP_RST_SW: return "software-reset";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt-watchdog";
    case ESP_RST_TASK_WDT: return "task-watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep-sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "sdio";
    default: return "unknown";
  }
}
}

namespace SystemDiagnostics {
void logBoot() {
  const esp_reset_reason_t reason = esp_reset_reason();
  Serial.printf("[BOOT] reset-reason=%d (%s)\\n", (int)reason, resetReasonName(reason));
  Serial.printf("[BOOT][HEALTH] free-heap=%lu min-free-heap=%lu largest-block=%u stack-watermark=%u words\\n",
                (unsigned long)ESP.getFreeHeap(),
                (unsigned long)ESP.getMinFreeHeap(),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                (unsigned)uxTaskGetStackHighWaterMark(nullptr));
}
}
''')

replace_once(
    "HP-print-server.ino",
    '#include "ipp_pcl3_service.h"\n',
    '#include "ipp_pcl3_service.h"\n#include "system_diagnostics.h"\n'
)
replace_once(
    "HP-print-server.ino",
    "  Serial.begin(115200);\n  delay(500);\n  Serial.println();",
    "  Serial.begin(115200);\n  delay(500);\n  Serial.println();\n  SystemDiagnostics::logBoot();"
)

print("Applied long-print stability batch successfully")
