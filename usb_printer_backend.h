#pragma once
#include <Arduino.h>
#include "mobile_print_queue.h"
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
  bool sendDirect(const uint8_t *data, size_t length, String &error);
  // Stream directly into USB using a bounded 8 KiB RAM buffer.
  bool sendStream(Stream &source, size_t length, String &error);
  bool processNext(MobilePrintQueue &queue, String &error);
private:
  UsbHostManager &host_;
  PrinterState state_ = OFFLINE;
  String reason_ = "usb-host-not-initialized";
  bool configured_ = false;
};
