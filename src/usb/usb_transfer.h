#pragma once

#include <Arduino.h>
#include "usb_device.h"

class UsbTransferLayer {
public:
  enum class Result : uint8_t {
    OK,
    NOT_READY,
    INVALID_ARGUMENT,
    NO_BULK_OUT,
    SUBMIT_FAILED,
    TIMEOUT,
    DEVICE_GONE,
    TRANSFER_FAILED,
    BUSY
  };

  struct Config {
    uint32_t timeoutMs = 5000;
    size_t maxChunkSize = 4096;
  };

  explicit UsbTransferLayer(const Config &config = Config{});

  bool begin();
  void stop();
  bool attach(const UsbDeviceInfo &device);
  void detach();

  bool ready() const;
  bool busy() const { return busy_; }
  const UsbDeviceInfo *device() const { return attached_ ? &device_ : nullptr; }
  Result lastResult() const { return lastResult_; }
  const char *lastError() const { return lastError_.c_str(); }

  // The transfer implementation is deliberately single-owner: callers feed
  // data through write(), while the USB host client task remains the sole
  // owner of USB host event processing.
  Result write(const uint8_t *data, size_t length, size_t *accepted = nullptr);
  Result write(Stream &input, size_t *bytesSent = nullptr);

private:
  Config config_;
  UsbDeviceInfo device_;
  bool started_ = false;
  bool attached_ = false;
  bool busy_ = false;
  Result lastResult_ = Result::NOT_READY;
  String lastError_;
};
