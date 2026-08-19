#pragma once
#include <Arduino.h>
#include "usb_host_manager.h"

class UsbPrinterBackend {
public:
  enum PrinterState : uint8_t { OFFLINE, IDLE, PRINTING, ERROR };
  explicit UsbPrinterBackend(UsbHostManager &host) : host_(host) {}
  bool begin();
  void poll();
  PrinterState state() const { return state_; }
  bool online() const { return state_ == IDLE || state_ == PRINTING; }
  const String &statusReason() const { return reason_; }
  const UsbDeviceInfo &device() const { return host_.device(); }

  // Send an already-rendered raw print stream byte-for-byte to USB Bulk OUT.
  bool sendDirect(const uint8_t *data, size_t length, String &error);

  // Called when a TCP/9100 job has ended. This deliberately does not alter the
  // document or append a form-feed/PJL command; RAW mode must remain byte exact.
  void finishRawJob();

private:
  UsbHostManager &host_;
  PrinterState state_ = OFFLINE;
  String reason_ = "usb-host-not-initialized";
  bool configured_ = false;
};
