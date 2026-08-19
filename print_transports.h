#pragma once
#include <Arduino.h>
#include <WiFiClient.h>
#include "printer_protocols.h"

struct PrintTarget {
  String host;
  IPAddress address;
  uint16_t port = 0;
  String queue;
};

class Raw9100Transport {
public:
  // JetDirect/AppSocket: sends an already-rendered print stream byte-for-byte.
  bool send(const PrintTarget& target, const uint8_t* data, size_t length,
            uint32_t timeoutMs = 10000);
  bool sendStream(const PrintTarget& target, Stream& source, size_t length,
                  uint32_t timeoutMs = 10000);
};

class IppTransport {
public:
  // Builds a complete IPP 1.1 Print-Job request. The document is not converted.
  bool buildPrintJobRequest(uint8_t* buffer, size_t capacity, size_t& length,
                            const String& printerUri, const String& user,
                            uint32_t jobId, const uint8_t* document,
                            size_t documentLength);

  // Sends a Print-Job to a network IPP printer and checks the HTTP + IPP response.
  bool send(const PrintTarget& target, const String& printerUri,
            const String& user, const String& documentFormat,
            const uint8_t* document, size_t documentLength,
            uint32_t timeoutMs = 15000, uint16_t* ippStatus = nullptr);

  // Streaming variant for LittleFS/queue-backed jobs; avoids a 2 MiB RAM buffer.
  bool sendStream(const PrintTarget& target, const String& printerUri,
                  const String& user, const String& documentFormat,
                  Stream& source, size_t documentLength,
                  uint32_t timeoutMs = 15000, uint16_t* ippStatus = nullptr);
};
