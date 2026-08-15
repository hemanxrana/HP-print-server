#pragma once
#include <Arduino.h>
#include <WiFi.h>

class MobileIppServer {
public:
  using JobHandler = bool (*)(const uint8_t *document, size_t length,
                               const String &documentFormat, uint32_t &jobId,
                               String &error);

  explicit MobileIppServer(uint16_t port = 631);
  void begin(const String &printerName, const String &printerUri, JobHandler handler);
  void poll();
  bool running() const { return running_; }

private:
  WiFiServer server_;
  uint16_t port_;
  bool running_ = false;
  String printerName_;
  String printerUri_;
  JobHandler handler_ = nullptr;
  uint32_t nextJobId_ = 1;

  void handleClient(WiFiClient &client);
  bool readHttpBody(WiFiClient &client, uint8_t **body, size_t &length);
  bool buildResponse(const uint8_t *request, size_t length, uint8_t *response,
                     size_t capacity, size_t &responseLength);
};
