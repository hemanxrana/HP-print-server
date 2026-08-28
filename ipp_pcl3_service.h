#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include "usb_printer_backend.h"

class IppPcl3Service {
public:
  explicit IppPcl3Service(UsbPrinterBackend &printer) : printer_(printer) {}
  void begin();
  void poll();
  bool active() const { return clientActive_; }
  uint64_t lastJobBytes() const { return lastJobBytes_; }
  const String &lastError() const { return lastError_; }

private:
  void handleClient(WiFiClient client);
  void refreshJobState();

  UsbPrinterBackend &printer_;
  bool started_ = false;
  bool clientActive_ = false;
  uint64_t lastJobBytes_ = 0;
  String lastError_;
  uint32_t nextJobId_ = 1;
  uint32_t lastJobId_ = 0;
  uint8_t lastJobState_ = 0; // IPP job-state enum: 5 processing, 8 aborted, 9 completed
  String lastJobReason_;
};
