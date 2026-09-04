#include "system_diagnostics.h"
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
  Serial.printf("[BOOT] reset-reason=%d (%s)\n", (int)reason, resetReasonName(reason));
  Serial.printf("[BOOT][HEALTH] free-heap=%lu min-free-heap=%lu largest-block=%u stack-watermark=%u words\n",
                (unsigned long)ESP.getFreeHeap(),
                (unsigned long)ESP.getMinFreeHeap(),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                (unsigned)uxTaskGetStackHighWaterMark(nullptr));
}
}
