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
  bool rawClientConnected() const { return rawClientConnected_; }
  uint64_t rawBytesReceived() const { return rawBytesReceived_; }

  bool usbStatusValid() const { return host_.statusValid(); }
  uint8_t usbPortStatus() const { return host_.portStatus(); }
  bool usbStatusInterfaceAvailable() const { return host_.statusInterfaceAvailable(); }

  // Send an already-rendered raw print stream byte-for-byte to USB Bulk OUT.
  bool sendDirect(const uint8_t *data, size_t length, String &error);

  // Called when a TCP/9100 job has ended. This deliberately does not alter the
  // document or append a form-feed/PJL command; RAW mode remains byte exact.
  void finishRawJob();

  void setRawClientConnected(bool connected) { rawClientConnected_ = connected; }
  void addRawBytes(uint64_t bytes) { rawBytesReceived_ += bytes; }
  void clearRawBytes() { rawBytesReceived_ = 0; }

private:
  UsbHostManager &host_;
  PrinterState state_ = OFFLINE;
  String reason_ = "usb-host-not-initialized";
  bool configured_ = false;
  bool rawClientConnected_ = false;
  uint64_t rawBytesReceived_ = 0;
};
