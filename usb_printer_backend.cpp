#include "usb_printer_backend.h"
#include <USB.h>
#include <USBHIDParser.h>

// This backend deliberately separates the ESP32-S3 USB Host transport from
// printer-language conversion. The queue owns the document; this class owns
// USB lifecycle and job state transitions. A printer-specific transport can
// be attached here without changing IPP.

bool UsbPrinterBackend::isPrinterClass(uint8_t cls, uint8_t sub, uint8_t proto) const {
  // USB Printer Class: class 0x07. Protocol 1 is unidirectional, 2 is
  // bidirectional. Protocol 0 is vendor-specific and is handled by the
  // discovery fallback in a future device driver.
  return cls == 0x07 && (sub == 0x01 || sub == 0x00) && (proto == 0x01 || proto == 0x02 || proto == 0x00);
}

bool UsbPrinterBackend::beginHost(String &error) {
  // Arduino-ESP32 exposes USB.begin() for the native USB Host stack. Actual
  // printer endpoints are intentionally not guessed: endpoint addresses and
  // max packet sizes must come from the connected device descriptors.
  if (!USB.begin()) {
    error = "ESP32-S3 USB host initialization failed";
    return false;
  }
  return true;
}

bool UsbPrinterBackend::discoverPrinter(String &error) {
  // Enumeration is asynchronous. USB Host callbacks/descriptor handling must
  // identify the interface before a bulk OUT transfer is attempted. The
  // generic Arduino USB class API does not provide a safe, portable printer
  // bulk transport abstraction across all Arduino-ESP32 releases, so this
  // layer remains OFFLINE until a printer-class driver registers its endpoints.
  // This is preferable to sending bytes to an arbitrary USB endpoint.
  error = "No USB Printer Class driver registered";
  return false;
}

bool UsbPrinterBackend::begin() {
  String error;
  configured_ = beginHost(error);
  if (!configured_) {
    state_ = OFFLINE;
    reason_ = error;
    return false;
  }
  if (!discoverPrinter(error)) {
    state_ = OFFLINE;
    reason_ = error;
    return false;
  }
  state_ = IDLE;
  reason_ = "printer-ready";
  return true;
}

void UsbPrinterBackend::poll() {
  // USB Host processing is performed by the Arduino-ESP32 USB stack. This
  // method is kept as the single integration point for device attach/detach
  // and printer status events once the class driver is attached.
}

bool UsbPrinterBackend::sendJob(MobilePrintQueue &queue, uint32_t jobId, String &error) {
  // Never claim completion without a real printer transport. A future
  // PrinterClassDriver will replace this function with descriptor-derived
  // bulk OUT transfers and printer status/error polling.
  (void)queue;
  (void)jobId;
  error = "USB Printer Class transport is not registered";
  return false;
}

bool UsbPrinterBackend::processNext(MobilePrintQueue &queue, String &error) {
  if (!online()) { error = reason_; return false; }
  uint32_t id = queue.firstPendingId();
  if (!id) { error = "No pending print job"; return false; }
  if (!queue.setState(id, MobilePrintQueue::STATE_PROCESSING, "printer-processing", error)) return false;
  state_ = PRINTING;
  reason_ = "printing";
  if (!sendJob(queue, id, error)) {
    String ignored;
    queue.setState(id, MobilePrintQueue::STATE_ABORTED, "printer-transport-error", ignored);
    state_ = ERROR;
    reason_ = error;
    return false;
  }
  if (!queue.setState(id, MobilePrintQueue::STATE_COMPLETED, "printer-completed", error)) {
    state_ = ERROR;
    reason_ = error;
    return false;
  }
  state_ = IDLE;
  reason_ = "printer-ready";
  return true;
}
