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

  uint8_t ippInterfaceCount() const;
  const UsbPrinterInterfaceInfo *ippInterfaceAt(uint8_t index) const;
  int8_t selectedIppInterfaceIndex() const { return device_.ippSelectedIndex; }
  bool selectIppInterface(uint8_t index, String &error);

  bool bulkWrite(const uint8_t *data, size_t length, size_t &accepted,
                 uint32_t timeoutMs, String &error);
  // Poll the selected classic Printer Class Bulk-IN endpoint. A normal USB
  // transfer timeout means "no backchannel bytes available yet" and returns
  // true with received == 0; hard USB errors return false.
  bool bulkRead(uint8_t *data, size_t capacity, size_t &received,
                uint32_t timeoutMs, String &error);
  bool ippBulkWrite(const uint8_t *data, size_t length, size_t &accepted,
                    uint32_t timeoutMs, String &error);
  bool ippBulkRead(uint8_t *data, size_t capacity, size_t &received,
                   uint32_t timeoutMs, String &error);
  // Interactive IPP bridge poll: a normal transfer timeout means that the
  // printer has no response bytes right now and returns true with received=0.
  bool ippBulkReadPoll(uint8_t *data, size_t capacity, size_t &received,
                       uint32_t timeoutMs, String &error);

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
