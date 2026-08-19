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
  const UsbPrinterInterfaceInfo *statusInterface() const;
  uint8_t interfaceCount() const { return device_.printerInterfaceCount; }
  const UsbPrinterInterfaceInfo *interfaceAt(uint8_t index) const;

  // RAW printing path.
  bool bulkWrite(const uint8_t *data, size_t length, size_t &accepted,
                 uint32_t timeoutMs, String &error);

  // USB Printer Class GET_PORT_STATUS. The request is performed by the USB
  // client task so it cannot interfere with the Arduino/Wi-Fi task.
  bool portStatusValid() const { return device_.portStatus.valid; }
  uint8_t portStatusValue() const { return device_.portStatus.value; }
  bool portStatusError() const { return device_.portStatus.error; }
  bool portStatusSelected() const { return device_.portStatus.selected; }
  bool portStatusPaperEmpty() const { return device_.portStatus.paperEmpty; }
  const UsbPortStatus &portStatus() const { return device_.portStatus; }
  bool hasSeparateStatusInterface() const { return device_.statusInterfaceSeparate; }

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
