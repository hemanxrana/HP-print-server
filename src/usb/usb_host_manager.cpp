#include "usb_host_manager.h"
#include <USB.h>

bool UsbHostManager::begin() {
  if (!USB.begin()) {
    state_ = ERROR;
    error_ = "USB host initialization failed";
    return false;
  }
  state_ = RUNNING;
  error_.clear();
  return true;
}

void UsbHostManager::poll() {
  // Arduino-ESP32 owns the native USB Host event processing. The adapter that
  // uses the exact installed core API must feed enumeration results through
  // onDeviceAttached()/onDeviceDetached(). We intentionally do not guess
  // endpoint addresses here.
}

void UsbHostManager::onDeviceAttached(const UsbDeviceInfo &info) {
  device_ = info;
  device_.attached = true;
  state_ = info.printer.found ? PRINTER_READY : DEVICE_ATTACHED;
  error_.clear();
}

void UsbHostManager::onDeviceDetached() {
  device_ = UsbDeviceInfo{};
  state_ = RUNNING;
}
