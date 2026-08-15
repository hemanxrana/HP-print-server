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
  // Descriptor enumeration is now real, but data transport is intentionally
  // not claimed yet. This prevents the state machine from reporting a print
  // as completed until a real USB Printer Class transfer is implemented.
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

  const uint32_t id = queue.firstPendingId();
  if (!id) {
    error = "No pending print job";
    return false;
  }

  if (!queue.setState(id, MobilePrintQueue::STATE_PROCESSING, "printer-processing", error)) {
    return false;
  }

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
