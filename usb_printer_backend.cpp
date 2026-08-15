#include "usb_printer_backend.h"

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
      state_ = PRINTING == state_ ? PRINTING : IDLE;
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
  (void)queue;
  (void)jobId;
  error = "USB Printer Class bulk transport is not implemented yet";
  return false;
}

bool UsbPrinterBackend::processNext(MobilePrintQueue &queue, String &error) {
  poll();
  if (!online()) {
    error = reason_;
    return false;
  }

  // Do not mutate persistent job state until a real transfer implementation
  // exists. This prevents a future caller from turning the current transport
  // placeholder into PROCESSING -> ABORTED churn.
  error = "USB Printer Class bulk transport is not implemented yet";
  return false;
}
