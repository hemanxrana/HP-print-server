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

  // USB Printer Class status, sourced from the descriptor-selected status
  // interface via GET_PORT_STATUS on EP0.
  bool usbStatusValid() const { return host_.portStatusValid(); }
  uint8_t usbPortStatus() const { return host_.portStatusValue(); }
  bool usbStatusError() const { return host_.portStatusError(); }
  bool usbStatusSelected() const { return host_.portStatusSelected(); }
  bool usbPaperEmpty() const { return host_.portStatusPaperEmpty(); }
  bool usbStatusUsesSeparateInterface() const { return host_.hasSeparateStatusInterface(); }

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
