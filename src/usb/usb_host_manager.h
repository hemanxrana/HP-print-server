#pragma once
#include <Arduino.h>
#include "usb_device.h"

class UsbHostManager {
public:
  enum State : uint8_t {
    STOPPED,
    RUNNING,
    ENUMERATING,
    DEVICE_ATTACHED,
    PRINTER_READY,
    ERROR
  };

  bool begin();
  void poll();
  State state() const { return state_; }
  const UsbDeviceInfo &device() const { return device_; }
  const String &lastError() const { return error_; }

  // Interface selection is configuration, not endpoint configuration. The
  // endpoints are always discovered from the selected interface descriptor.
  void setInterfaceSelection(bool automatic, uint8_t interfaceNumber, uint8_t alternateSetting);
  bool automaticInterfaceSelection() const { return autoSelect_; }
  uint8_t manualInterfaceNumber() const { return manualInterface_; }
  uint8_t manualAlternateSetting() const { return manualAlt_; }
  const UsbPrinterInterfaceInfo *selectedInterface() const;
  uint8_t interfaceCount() const { return device_.printerInterfaceCount; }
  const UsbPrinterInterfaceInfo *interfaceAt(uint8_t index) const;

  // Called by the application/print backend. USB host events continue to be
  // owned by clientTask(); this method only submits a transfer and waits for
  // its completion callback.
  bool bulkWrite(const uint8_t *data, size_t length, size_t &accepted, uint32_t timeoutMs, String &error);

  // USB client task notification points. These are not USB callbacks; the
  // single client task calls them after descriptor work.
  void onEnumerated(const UsbDeviceInfo &info);
  void onDetached();
  void onEnumerationError(const String &error);

private:
  State state_ = STOPPED;
  UsbDeviceInfo device_;
  String error_;
  bool started_ = false;
  bool autoSelect_ = true;
  uint8_t manualInterface_ = 0;
  uint8_t manualAlt_ = 0;
};
