#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include "usb_printer_backend.h"

class IppPcl3Service {
public:
  explicit IppPcl3Service(UsbPrinterBackend &printer) : printer_(printer) {}
  static String printerUuidBare();
  void begin();
  void rebind();
  void poll();
  bool active() const { return clientActive_; }
  uint64_t lastJobBytes() const { return lastJobBytes_; }
  const String &lastError() const { return lastError_; }
  uint32_t lastJobId() const { return lastJobId_; }
  uint8_t lastJobState() const { return lastJobState_; }
  const String &lastJobReason() const { return lastJobReason_; }
private:
  void handleClient(WiFiClient &client);
  void refreshJobState();
  void trackPrinterState();

  UsbPrinterBackend &printer_;
  bool started_ = false;
  bool clientActive_ = false;
  uint64_t lastJobBytes_ = 0;
  String lastError_;

  uint32_t nextJobId_ = 1;
  uint32_t lastJobId_ = 0;
  // IPP job-state enum: 3 pending, 4 processing, 6 stopped, 7 canceled,
  // 8 aborted, 9 completed.
  uint8_t lastJobState_ = 0;
  String lastJobReason_;
  String lastJobName_;
  bool pendingJob_ = false;          // Create-Job seen, document not yet sent
  bool jobStartedProcessing_ = false;
  uint32_t jobProcessingUpTime_ = 0;
  uint32_t jobCompletedUpTime_ = 0;

  // For printer-state-change-time / config-change-time.
  uint8_t observedPrinterState_ = 0xFF;
  uint32_t stateChangeUpTime_ = 0;
  uint32_t configChangeUpTime_ = 0;
};
