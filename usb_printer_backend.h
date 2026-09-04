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
  bool online() const { return state_ == IDLE; }
  const String &statusReason() const { return reason_; }
  const UsbDeviceInfo &device() const { return host_.device(); }
  bool rawClientConnected() const;

  bool usbStatusValid() const { return host_.portStatusValid(); }
  uint8_t usbPortStatus() const { return host_.portStatusValue(); }
  bool usbStatusError() const { return usbStatusValid() && !(usbPortStatus() & 0x08); }
  bool usbStatusSelected() const { return usbStatusValid() && (usbPortStatus() & 0x10); }
  bool usbPaperEmpty() const { return usbStatusValid() && (usbPortStatus() & 0x20); }
  bool usbStatusUsesSeparateInterface() const { return host_.hasSeparateStatusInterface(); }

  bool sendDirect(const uint8_t *data, size_t length, String &error);
  void finishRawJob();
  void abortRawJob(const String &reason);

private:
  UsbPrinterBackend(const UsbPrinterBackend &) = delete;
  UsbPrinterBackend &operator=(const UsbPrinterBackend &) = delete;

  void completeDrainIfReady();

  UsbHostManager &host_;
  PrinterState state_ = OFFLINE;
  String reason_ = "usb-host-not-initialized";
  uint64_t jobBytes_ = 0;
  uint64_t nextHealthLogAt_ = 512ULL * 1024ULL;
  bool drainPending_ = false;
  unsigned long drainUntilMs_ = 0;
  unsigned long drainStartedMs_ = 0;
  uint32_t drainStatusAtStart_ = 0;
};
