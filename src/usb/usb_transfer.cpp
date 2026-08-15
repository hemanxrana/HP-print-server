#include "usb_transfer.h"

#include <algorithm>
#include <cstring>

UsbTransferLayer::UsbTransferLayer(const Config &config) : config_(config) {
  if (config_.timeoutMs == 0) config_.timeoutMs = 5000;
  if (config_.maxChunkSize == 0) config_.maxChunkSize = 4096;
}

bool UsbTransferLayer::begin() {
  started_ = true;
  lastResult_ = attached_ ? Result::OK : Result::NOT_READY;
  lastError_.clear();
  return true;
}

void UsbTransferLayer::stop() {
  busy_ = false;
  attached_ = false;
  device_ = UsbDeviceInfo{};
  started_ = false;
  lastResult_ = Result::NOT_READY;
  lastError_ = "transfer layer stopped";
}

bool UsbTransferLayer::attach(const UsbDeviceInfo &device) {
  if (!started_) {
    lastResult_ = Result::NOT_READY;
    lastError_ = "transfer layer not started";
    return false;
  }
  if (!device.attached || !device.printer.found || !device.printer.bulkOut.valid() ||
      !device.printer.bulkOut.isBulk() || device.printer.bulkOut.isIn()) {
    lastResult_ = Result::NO_BULK_OUT;
    lastError_ = "printer has no valid Bulk OUT endpoint";
    return false;
  }

  device_ = device;
  attached_ = true;
  lastResult_ = Result::OK;
  lastError_.clear();
  return true;
}

void UsbTransferLayer::detach() {
  busy_ = false;
  attached_ = false;
  device_ = UsbDeviceInfo{};
  lastResult_ = Result::DEVICE_GONE;
  lastError_ = "USB printer detached";
}

bool UsbTransferLayer::ready() const {
  return started_ && attached_ && device_.printer.found && device_.printer.bulkOut.valid();
}

UsbTransferLayer::Result UsbTransferLayer::write(const uint8_t *data, size_t length, size_t *accepted) {
  if (accepted) *accepted = 0;
  if (!ready()) {
    lastResult_ = Result::NOT_READY;
    lastError_ = "USB printer transfer is not ready";
    return lastResult_;
  }
  if (!data && length != 0) {
    lastResult_ = Result::INVALID_ARGUMENT;
    lastError_ = "null data pointer";
    return lastResult_;
  }
  if (busy_) {
    lastResult_ = Result::BUSY;
    lastError_ = "another transfer is active";
    return lastResult_;
  }
  if (length == 0) {
    lastResult_ = Result::OK;
    lastError_.clear();
    return lastResult_;
  }

  // This method is intentionally a transport boundary, not a fake printer
  // implementation. The actual usb_host_transfer_submit() call belongs to the
  // USB host client/task that owns the endpoint and its transfer callback.
  // Until that endpoint handle is wired into the host runtime, claiming that
  // bytes reached the printer would make the persistent job state incorrect.
  busy_ = true;
  lastResult_ = Result::SUBMIT_FAILED;
  lastError_ = "USB bulk transfer backend is not yet bound to the enumerated endpoint";
  busy_ = false;
  return lastResult_;
}

UsbTransferLayer::Result UsbTransferLayer::write(Stream &input, size_t *bytesSent) {
  if (bytesSent) *bytesSent = 0;
  if (!ready()) {
    lastResult_ = Result::NOT_READY;
    lastError_ = "USB printer transfer is not ready";
    return lastResult_;
  }

  size_t total = 0;
  const size_t chunk = std::min(config_.maxChunkSize, static_cast<size_t>(8192));
  uint8_t *buffer = static_cast<uint8_t *>(malloc(chunk));
  if (!buffer) {
    lastResult_ = Result::TRANSFER_FAILED;
    lastError_ = "unable to allocate transfer buffer";
    return lastResult_;
  }

  while (input.available() > 0) {
    const int n = input.read(buffer, chunk);
    if (n <= 0) {
      free(buffer);
      lastResult_ = Result::TRANSFER_FAILED;
      lastError_ = "input stream read failed";
      return lastResult_;
    }

    size_t accepted = 0;
    const Result result = write(buffer, static_cast<size_t>(n), &accepted);
    total += accepted;
    if (result != Result::OK) {
      free(buffer);
      if (bytesSent) *bytesSent = total;
      return result;
    }
  }

  free(buffer);
  if (bytesSent) *bytesSent = total;
  lastResult_ = Result::OK;
  lastError_.clear();
  return lastResult_;
}
