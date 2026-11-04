#pragma once
#include <Arduino.h>
#include "usb_host_manager.h"

// Dedicated USB Bulk OUT writer.
//
// The RAW 9100 and IPP servers both run inside the Arduino loop task. Handing
// bytes straight to usb_host_transfer_submit() there means a slow printer
// (buffer full) stalls HTTP, mDNS and status polling for the whole job. This
// class moves the submit/drain work onto its own task: producers copy into a
// PSRAM ring and only block when the ring is genuinely full, which is real
// backpressure rather than loop starvation.
//
// Single producer (loop task) / single consumer (writer task).
class UsbStream {
public:
  bool begin(UsbHostManager &host);
  bool ready() const { return task_ != nullptr; }

  // Queue print data. Blocks only while the staging ring is full.
  bool push(const uint8_t *data, size_t length, uint32_t timeoutMs);

  // Wait until every queued byte has been accepted by the USB host driver and
  // the pipelined transfers have drained. Must be called by the producer once
  // the job's last byte has been pushed.
  bool flush(uint32_t timeoutMs, String &error);

  // Drop queued data and clear the sticky transport error. Called when a new
  // job starts.
  void reset();

  bool failed(String &error) const;
  size_t backlog() const { return used_; }

private:
  static void taskEntry(void *arg);
  void run();
  bool copyOut(uint8_t *dst, size_t max, size_t &taken);
  void setError(const char *msg);

  UsbHostManager *host_ = nullptr;
  uint8_t *ring_ = nullptr;
  uint8_t *staging_ = nullptr;
  size_t capacity_ = 0;

  volatile size_t head_ = 0;   // producer write index
  volatile size_t tail_ = 0;   // consumer read index
  volatile size_t used_ = 0;   // bytes queued
  volatile bool busy_ = false; // consumer owns the USB pipeline
  volatile bool failed_ = false;

  char error_[96] = {0};
  TaskHandle_t task_ = nullptr;
  SemaphoreHandle_t wake_ = nullptr;
  mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;

  UsbStream(const UsbStream &) = delete;
  UsbStream &operator=(const UsbStream &) = delete;

public:
  UsbStream() = default;
};
