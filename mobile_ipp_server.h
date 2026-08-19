#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include "mobile_print_queue.h"

class MobileIppServer {
public:
  using JobHandler = bool (*)(Stream &document, size_t documentLength,
                              const String &documentFormat, uint32_t &jobId,
                              String &error);
  explicit MobileIppServer(uint16_t port = 631);
  void begin(const String &printerName, const String &printerUri,
             JobHandler handler, MobilePrintQueue *queue);
  void poll();
  bool running() const { return running_; }
private:
  WiFiServer server_;
  uint16_t port_;
  bool running_ = false;
  String printerName_, printerUri_, printerPath_;
  JobHandler handler_ = nullptr;
  MobilePrintQueue *queue_ = nullptr;
  void handleClient(WiFiClient &client);
};
