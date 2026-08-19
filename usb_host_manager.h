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

  void setInterfaceSelection(bool automatic, uint8_t interfaceNumber, uint8_t alternateSetting);
  bool automaticInterfaceSelection() const { return autoSelect_; }
  uint8_t manualInterfaceNumber() const { return manualInterface_; }
  uint8_t manualAlternateSetting() const { return manualAlt_; }
  const UsbPrinterInterfaceInfo *selectedInterface() const;
  uint8_t interfaceCount() const { return device_.printerInterfaceCount; }
  const UsbPrinterInterfaceInfo *interfaceAt(uint8_t index) const;

  // Request an asynchronous USB Printer Class GET_PORT_STATUS refresh.
  // The request is executed by the USB host client task so the client handle
  // is never used concurrently by the web/main loop.
  void requestStatusRefresh();
  bool statusValid() const { return device_.portStatusValid; }
  uint8_t portStatus() const { return device_.portStatus; }
  bool statusInterfaceAvailable() const { return device_.statusInterfaceFound; }

  bool bulkWrite(const uint8_t *data, size_t length, size_t &accepted,
                 uint32_t timeoutMs, String &error);

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
