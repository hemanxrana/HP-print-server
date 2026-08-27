#pragma once
#include <Arduino.h>
#include "usb_host_manager.h"

void ensureUsbScannerBackendStarted();
void ensureUsbScannerWebRoutesInstalled();

class UsbPrinterBackend {
public:
  enum PrinterState : uint8_t { OFFLINE, IDLE, PRINTING, ERROR };
  explicit UsbPrinterBackend(UsbHostManager &host) : host_(host) {}
  bool begin();
  void poll();
  PrinterState state() const {
    // Lazy-start the independent scanner client only after the normal Arduino
    // setup path has installed the USB host library. Scanner failure is
    // intentionally non-fatal to printing. The browser fallback routes are
    // installed alongside the scanner service and do not alter print handling.
    ensureUsbScannerBackendStarted();
    ensureUsbScannerWebRoutesInstalled();
    return state_;
  }
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
  bool drainPending_ = false;
  unsigned long drainUntilMs_ = 0;
};
