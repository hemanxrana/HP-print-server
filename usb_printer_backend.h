#pragma once
#include <Arduino.h>
#include "mobile_print_queue.h"

class UsbPrinterBackend {
public:
  enum PrinterState : uint8_t { OFFLINE, IDLE, PRINTING, ERROR };
  bool begin();
  void poll();
  PrinterState state() const { return state_; }
  bool online() const { return state_ != OFFLINE; }
  const String &statusReason() const { return reason_; }
  bool processNext(MobilePrintQueue &queue, String &error);

private:
  PrinterState state_ = OFFLINE;
  String reason_ = "usb-host-not-initialized";
  uint16_t vid_ = 0;
  uint16_t pid_ = 0;
  bool configured_ = false;
  bool beginHost(String &error);
  bool discoverPrinter(String &error);
  bool sendJob(MobilePrintQueue &queue, uint32_t jobId, String &error);
  bool isPrinterClass(uint8_t interfaceClass, uint8_t interfaceSubclass, uint8_t interfaceProtocol) const;
};
