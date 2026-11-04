#include "usb_stream.h"
#include "logger.h"
#include "config.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <cstring>

namespace {
constexpr size_t STREAM_CHUNK = BULK_OUT_CHUNK_SIZE;
constexpr uint32_t RESET_BUSY_WAIT_MS = 1000;
}  // namespace

bool UsbStream::begin(UsbHostManager &host) {
  if (task_) return true;
  host_ = &host;
  capacity_ = USB_STREAM_BUFFER_SIZE;

  ring_ = static_cast<uint8_t *>(heap_caps_malloc(capacity_, MALLOC_CAP_SPIRAM));
  if (!ring_) ring_ = static_cast<uint8_t *>(malloc(capacity_));
  staging_ = static_cast<uint8_t *>(heap_caps_malloc(STREAM_CHUNK, MALLOC_CAP_SPIRAM));
  if (!staging_) staging_ = static_cast<uint8_t *>(malloc(STREAM_CHUNK));

  if (!ring_ || !staging_) {
    Log.println("[USB] Stream buffer allocation failed; using synchronous USB path");
    free(ring_);
    free(staging_);
    ring_ = nullptr;
    staging_ = nullptr;
    return false;
  }

  wake_ = xSemaphoreCreateBinary();
  if (!wake_) {
    Log.println("[USB] Stream semaphore allocation failed; using synchronous USB path");
    free(ring_);
    free(staging_);
    ring_ = nullptr;
    staging_ = nullptr;
    return false;
  }

  if (xTaskCreatePinnedToCore(taskEntry, "usbstream", USB_STREAM_TASK_STACK, this,
                              USB_STREAM_TASK_PRIORITY, &task_,
                              USB_STREAM_TASK_CORE) != pdPASS) {
    task_ = nullptr;
    vSemaphoreDelete(wake_);
    wake_ = nullptr;
    free(ring_);
    free(staging_);
    ring_ = nullptr;
    staging_ = nullptr;
    Log.println("[USB] Stream task create failed; using synchronous USB path");
    return false;
  }

  Log.printf("[USB] Stream writer task started: ring=%u chunk=%u pipeline=%u core=%u\n",
             (unsigned)capacity_, (unsigned)STREAM_CHUNK,
             (unsigned)USB_BULK_PIPELINE_DEPTH, (unsigned)USB_STREAM_TASK_CORE);
  return true;
}

void UsbStream::setError(const char *msg) {
  portENTER_CRITICAL(&mux_);
  strncpy(error_, msg ? msg : "", sizeof(error_) - 1);
  error_[sizeof(error_) - 1] = 0;
  portEXIT_CRITICAL(&mux_);
}

bool UsbStream::failed(String &error) const {
  if (!failed_) return false;
  char tmp[sizeof(error_)];
  portENTER_CRITICAL(&mux_);
  memcpy(tmp, error_, sizeof(tmp));
  portEXIT_CRITICAL(&mux_);
  error = tmp;
  if (!error.length()) error = "USB stream failed";
  return true;
}

bool UsbStream::push(const uint8_t *data, size_t length, uint32_t timeoutMs) {
  if (!task_) {
    setError("USB stream task not running");
    return false;
  }
  if (!data || !length) return true;

  String failure;
  const unsigned long start = millis();
  size_t offset = 0;

  while (offset < length) {
    if (failed(failure)) return false;

    size_t head = 0;
    size_t space = 0;
    portENTER_CRITICAL(&mux_);
    head = head_;
    space = capacity_ - used_;
    portEXIT_CRITICAL(&mux_);

    if (space == 0) {
      if (timeoutMs && (unsigned long)(millis() - start) >= timeoutMs) {
        setError("USB stream backpressure timeout");
        return false;
      }
      esp_task_wdt_reset();
      delay(1);
      continue;
    }

    const size_t n = (space < (length - offset)) ? space : (length - offset);
    const size_t first = (n < (capacity_ - head)) ? n : (capacity_ - head);
    memcpy(ring_ + head, data + offset, first);
    if (n > first) memcpy(ring_, data + offset + first, n - first);

    // Publish only after the copy so the consumer never sees unwritten bytes.
    portENTER_CRITICAL(&mux_);
    head_ = (head + n) % capacity_;
    used_ += n;
    portEXIT_CRITICAL(&mux_);

    offset += n;
  }

  xSemaphoreGive(wake_);
  return true;
}

bool UsbStream::copyOut(uint8_t *dst, size_t max, size_t &taken) {
  taken = 0;
  size_t tail = 0;
  size_t avail = 0;
  portENTER_CRITICAL(&mux_);
  tail = tail_;
  avail = used_;
  portEXIT_CRITICAL(&mux_);

  if (avail == 0) return false;

  taken = (avail < max) ? avail : max;
  const size_t first = (taken < (capacity_ - tail)) ? taken : (capacity_ - tail);
  memcpy(dst, ring_ + tail, first);
  if (taken > first) memcpy(dst + first, ring_, taken - first);

  portENTER_CRITICAL(&mux_);
  tail_ = (tail + taken) % capacity_;
  used_ -= taken;
  portEXIT_CRITICAL(&mux_);
  return true;
}

void UsbStream::run() {
  for (;;) {
    if (failed_) {
      xSemaphoreTake(wake_, portMAX_DELAY);
      continue;
    }

    // busy_ is raised before the ring is touched; flush() relies on this
    // ordering to decide the pipeline is idle.
    busy_ = true;
    size_t taken = 0;
    if (!copyOut(staging_, STREAM_CHUNK, taken) || taken == 0) {
      busy_ = false;
      xSemaphoreTake(wake_, portMAX_DELAY);
      continue;
    }

    String error;
    const bool ok = host_->bulkWriteAll(staging_, taken, USB_STREAM_PUSH_TIMEOUT_MS, error);
    busy_ = false;
    esp_task_wdt_reset();

    if (!ok) {
      setError(error.length() ? error.c_str() : "USB bulk stream failed");
      failed_ = true;
      Log.printf("[USB][STREAM] bulk write failed: %s\n", error.c_str());
    }
  }
}

void UsbStream::taskEntry(void *arg) {
  static_cast<UsbStream *>(arg)->run();
}

bool UsbStream::flush(uint32_t timeoutMs, String &error) {
  const unsigned long start = millis();

  for (;;) {
    String failure;
    if (failed(failure)) { error = failure; return false; }

    size_t used = 0;
    portENTER_CRITICAL(&mux_);
    used = used_;
    portEXIT_CRITICAL(&mux_);
    const bool busy = busy_;

    if (used == 0 && !busy) break;
    if (timeoutMs && (unsigned long)(millis() - start) >= timeoutMs) {
      error = "USB stream flush timeout";
      return false;
    }
    esp_task_wdt_reset();
    delay(1);
  }

  return host_->waitForBulkOut(timeoutMs, error);
}

void UsbStream::reset() {
  const unsigned long start = millis();
  while (busy_ && (unsigned long)(millis() - start) < RESET_BUSY_WAIT_MS) {
    esp_task_wdt_reset();
    delay(1);
  }

  portENTER_CRITICAL(&mux_);
  head_ = 0;
  tail_ = 0;
  used_ = 0;
  error_[0] = 0;
  portEXIT_CRITICAL(&mux_);

  failed_ = false;
  if (wake_) xSemaphoreGive(wake_);
}
