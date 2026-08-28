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

  UsbPrinterBackend &printer_;
  bool started_ = false;
  bool clientActive_ = false;
  uint64_t lastJobBytes_ = 0;
  String lastError_;
};
