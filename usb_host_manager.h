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

  const UsbPrinterInterfaceInfo *selectedInterface() const;
  const UsbPrinterInterfaceInfo *statusInterface() const;

  bool bulkWrite(const uint8_t *data, size_t length, size_t &accepted,
                 uint32_t timeoutMs, String &error);

  // Optional USB Printer Class Bulk-IN backchannel. These methods are
  // non-blocking and do not affect the existing Bulk-OUT print path.
  bool backchannelSupported() const;
  size_t backchannelAvailable() const;
  size_t readBackchannel(uint8_t *data, size_t capacity);
  void clearBackchannel();
  uint32_t backchannelDroppedBytes() const;

  bool portStatusValid() const { return device_.portStatus.valid; }
  uint8_t portStatusValue() const { return device_.portStatus.value; }
  bool portStatusError() const { return device_.portStatus.error; }
  bool portStatusSelected() const { return device_.portStatus.selected; }
  bool portStatusPaperEmpty() const { return device_.portStatus.paperEmpty; }
  const UsbPortStatus &portStatus() const { return device_.portStatus; }
  bool hasSeparateStatusInterface() const { return device_.statusInterfaceSeparate; }

  void onPortStatusTransfer(bool valid, uint8_t value, const String &error);
  void onEnumerationStarted();
  void onEnumerated(const UsbDeviceInfo &info);
  void onDetached();
  void onEnumerationError(const String &error);

private:
  State state_ = STOPPED;
  UsbDeviceInfo device_;
  String error_;
  bool started_ = false;
};
