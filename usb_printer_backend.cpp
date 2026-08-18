#include "usb_printer_backend.h"

namespace {
class UsbOutputStream : public Stream {
public:
  explicit UsbOutputStream(UsbHostManager &host) : host_(host) {}
  size_t write(uint8_t b) override { return write(&b, 1); }
  size_t write(const uint8_t *buffer, size_t size) override {
    if (!buffer || size == 0) return 0;
    size_t accepted = 0; String error;
    if (!host_.bulkWrite(buffer, size, accepted, 5000, error)) { error_ = error; return accepted; }
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

static bool rawProtocolSupported(const UsbDeviceInfo &d) {
  return d.printer.found && d.printer.protocol == 0x02 && d.printer.usableForRawPrint();
}

static bool sendPclTestPage(UsbHostManager &host, String &error) {
  static const uint8_t job[] = {
    0x1B, 0x45,
    0x1B, 0x26, 0x6C, 0x30, 0x4F,
    0x1B, 0x26, 0x6C, 0x36, 0x44,
    0x1B, 0x26, 0x6C, 0x30, 0x45,
    0x1B, 0x26, 0x61, 0x30, 0x4C,
    'H','P',' ','P','r','i','n','t',' ','S','e','r','v','e','r',' ','U','S','B',' ','T','e','s','t','\r','\n',
    'I','n','t','e','r','f','a','c','e',' ','a','n','d',' ','B','u','l','k',' ','O','U','T',' ','t','r','a','n','s','f','e','r',' ','O','K','.', '\r','\n',
    0x0C, 0x1B, 0x45
  };
  size_t accepted = 0;
  if (!host.bulkWrite(job, sizeof(job), accepted, 5000, error)) return false;
  if (accepted != sizeof(job)) { error = "PCL test page was only partially transferred"; return false; }
  return true;
}
}

bool UsbPrinterBackend::begin() {
  if (!host_.begin()) { configured_ = false; state_ = OFFLINE; reason_ = host_.lastError(); return false; }
  configured_ = true; state_ = OFFLINE; reason_ = "waiting-for-usb-printer"; return true;
}

void UsbPrinterBackend::poll() {
  if (!configured_) return;
  switch (host_.state()) {
    case UsbHostManager::PRINTER_READY: if (state_ != PRINTING) state_ = IDLE; reason_ = rawProtocolSupported(host_.device()) ? "printer-ready" : "printer-ready-but-selected-protocol-is-not-raw-PCL"; break;
    case UsbHostManager::DEVICE_ATTACHED:
    case UsbHostManager::ENUMERATING: if (state_ != PRINTING) state_ = OFFLINE; reason_ = "enumerating-usb-device"; break;
    case UsbHostManager::ERROR: if (state_ != PRINTING) state_ = ERROR; reason_ = host_.lastError(); break;
    default: if (state_ != PRINTING) state_ = OFFLINE; reason_ = host_.lastError().length() ? host_.lastError() : "waiting-for-usb-printer"; break;
  }
}

bool UsbPrinterBackend::sendJob(MobilePrintQueue &queue, uint32_t jobId, String &error) {
  if (!rawProtocolSupported(host_.device())) { error = "Selected USB interface is not the raw bidirectional Printer Class protocol (0x02)"; return false; }
  UsbOutputStream output(host_);
  if (!queue.readJob(jobId, output, error)) return false;
  if (output.error().length()) { error = output.error(); return false; }
  return true;
}

bool UsbPrinterBackend::processNext(MobilePrintQueue &queue, String &error) {
  poll();
  if (!online()) { error = reason_; return false; }
  const uint32_t jobId = queue.firstPendingId();
  if (!jobId) { error = "no pending print job"; return false; }
  if (!rawProtocolSupported(host_.device())) { error = "Selected interface is not a raw PCL transport; choose protocol 0x02 or implement IPP-over-USB separately"; return false; }
  if (!queue.setState(jobId, MobilePrintQueue::STATE_PROCESSING, "usb-transfer-started", error)) return false;
  state_ = PRINTING; reason_ = "printing-job-" + String(jobId);
  if (sendJob(queue, jobId, error)) {
    String stateError;
    if (!queue.setState(jobId, MobilePrintQueue::STATE_COMPLETED, "usb-transfer-complete", stateError)) { error = stateError; state_ = ERROR; reason_ = error; return false; }
    state_ = IDLE; reason_ = "printer-ready"; return true;
  }
  String stateError; queue.setState(jobId, MobilePrintQueue::STATE_ABORTED, error, stateError);
  state_ = ERROR; reason_ = error; return false;
}

bool UsbPrinterBackend::testPrint(String &error) {
  poll();
  if (!online()) { error = reason_; return false; }
  if (!rawProtocolSupported(host_.device())) { error = "Test Print requires the standard bidirectional Printer Class protocol 0x02; the selected interface is not a raw PCL transport"; return false; }
  state_ = PRINTING; reason_ = "gui-test-print";
  const bool ok = sendPclTestPage(host_, error);
  state_ = ok ? IDLE : ERROR; reason_ = ok ? "test-print-transferred" : error;
  return ok;
}
