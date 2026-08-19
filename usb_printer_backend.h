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

  // USB Printer Class GET_PORT_STATUS, sourced from the descriptor-selected
  // status interface. The raw byte is interpreted here using the USB Printer
  // Class definition: bit 5=paper empty, bit 4=selected, bit 3=not error.
  bool usbStatusValid() const { return host_.portStatusValid(); }
  uint8_t usbPortStatus() const { return host_.portStatusValue(); }
  bool usbStatusError() const { return usbStatusValid() && !(usbPortStatus() & 0x08); }
  bool usbStatusSelected() const { return usbStatusValid() && (usbPortStatus() & 0x10); }
  bool usbPaperEmpty() const { return usbStatusValid() && (usbPortStatus() & 0x20); }
  bool usbStatusUsesSeparateInterface() const { return host_.hasSeparateStatusInterface(); }

  // Send an already-rendered raw print stream byte-for-byte to USB Bulk OUT.
  bool sendDirect(const uint8_t *data, size_t length, String &error);

  // Called after TCP 9100 has been fully drained. This never adds print data.
  void finishRawJob();

private:
  UsbPrinterBackend(const UsbPrinterBackend &) = delete;
  UsbPrinterBackend &operator=(const UsbPrinterBackend &) = delete;

  UsbHostManager &host_;
  PrinterState state_ = OFFLINE;
  String reason_ = "usb-host-not-initialized";
  bool configured_ = false;
  uint64_t jobBytes_ = 0;
};
