#include "usb_printer_backend.h"

namespace {

class UsbOutputStream : public Stream {
public:
  explicit UsbOutputStream(UsbHostManager &host) : host_(host) {}

  size_t write(uint8_t b) override { return write(&b, 1); }

  size_t write(const uint8_t *buffer, size_t size) override {
    if (!buffer || size == 0) return 0;
    size_t accepted = 0;
    String error;
    if (!host_.bulkWrite(buffer, size, accepted, 5000, error)) {
      error_ = error;
      return accepted;
    }
    return accepted;
  }

  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}

  const String &error() const { return error_; }

private:
  UsbHostManager &host_;
  String error_;
};

static bool sendPclTestPage(UsbHostManager &host, String &error) {
  // This is intentionally a transport test generated at the production
  // backend boundary. It is not a standalone test sketch. The page uses
  // ordinary PCL text so we can first prove USB interface/transfer correctness
  // before implementing a PCL3GUI raster encoder.
  static const uint8_t job[] = {
    0x1B, 0x45,                         // ESC E: reset printer
    0x1B, 0x26, 0x6C, 0x30, 0x4F,     // ESC &l0O: portrait
    0x1B, 0x26, 0x6C, 0x36, 0x44,     // ESC &l6D: 6 lpi
    0x1B, 0x26, 0x6C, 0x30, 0x45,     // ESC &l0E: top margin
    0x1B, 0x26, 0x61, 0x30, 0x4C,     // ESC &a0L: left margin
    'H','P',' ','P','r','i','n','t',' ','S','e','r','v','e','r',' ','U','S','B',' ','T','e','s','t','\r','\n',
    'I','n','t','e','r','f','a','c','e',' ','a','n','d',' ','B','u','l','k',' ','O','U','T',' ','t','r','a','n','s','f','e','r',' ','O','K','.', '\r','\n',
    0x0C,                              // Form Feed: eject page
    0x1B, 0x45                          // reset/end
  };

  size_t accepted = 0;
  if (!host.bulkWrite(job, sizeof(job), accepted, 5000, error)) return false;
  if (accepted != sizeof(job)) {
    error = "PCL test page was only partially transferred";
    return false;
  }
  return true;
}

} // namespace

bool UsbPrinterBackend::begin() {
  if (!host_.begin()) {
    configured_ = false;
    state_ = OFFLINE;
    reason_ = host_.lastError();
    return false;
  }

  configured_ = true;
  state_ = OFFLINE;
  reason_ = "waiting-for-usb-printer";
  return true;
}

void UsbPrinterBackend::poll() {
  if (!configured_) return;

  switch (host_.state()) {
    case UsbHostManager::PRINTER_READY:
      if (state_ != PRINTING) state_ = IDLE;
      reason_ = "printer-ready";
      break;
    case UsbHostManager::DEVICE_ATTACHED:
    case UsbHostManager::ENUMERATING:
      if (state_ != PRINTING) state_ = OFFLINE;
      reason_ = "enumerating-usb-device";
      break;
    case UsbHostManager::ERROR:
      if (state_ != PRINTING) state_ = ERROR;
      reason_ = host_.lastError();
      break;
    case UsbHostManager::RUNNING:
    case UsbHostManager::STOPPED:
    default:
      if (state_ != PRINTING) state_ = OFFLINE;
      reason_ = host_.lastError().length() ? host_.lastError() : "waiting-for-usb-printer";
      break;
  }
}

bool UsbPrinterBackend::sendJob(MobilePrintQueue &queue, uint32_t jobId, String &error) {
  UsbOutputStream output(host_);
  if (!queue.readJob(jobId, output, error)) return false;
  if (output.error().length()) {
    error = output.error();
    return false;
  }
  return true;
}

bool UsbPrinterBackend::processNext(MobilePrintQueue &queue, String &error) {
  poll();
  if (!online()) {
    error = reason_;
    return false;
  }

  const uint32_t jobId = queue.firstPendingId();
  if (!jobId) {
    error = "no pending print job";
    return false;
  }

  if (!queue.setState(jobId, MobilePrintQueue::STATE_PROCESSING, "usb-transfer-started", error)) return false;
  state_ = PRINTING;
  reason_ = "printing-job-" + String(jobId);

  if (sendJob(queue, jobId, error)) {
    String stateError;
    if (!queue.setState(jobId, MobilePrintQueue::STATE_COMPLETED, "usb-transfer-complete", stateError)) {
      error = stateError;
      state_ = ERROR;
      reason_ = error;
      return false;
    }
    state_ = IDLE;
    reason_ = "printer-ready";
    return true;
  }

  String stateError;
  queue.setState(jobId, MobilePrintQueue::STATE_ABORTED, error, stateError);
  state_ = ERROR;
  reason_ = error;
  return false;
}

bool UsbPrinterBackend::testPrint(String &error) {
  poll();
  if (!online()) {
    error = reason_;
    return false;
  }

  state_ = PRINTING;
  reason_ = "gui-test-print";
  const bool ok = sendPclTestPage(host_, error);
  state_ = ok ? IDLE : ERROR;
  reason_ = ok ? "test-print-transferred" : error;
  return ok;
}
