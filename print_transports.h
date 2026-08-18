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
  bool send(const PrintTarget& target, const uint8_t* data, size_t length,
            uint32_t timeoutMs = 10000);
};

class IppTransport {
public:
  bool buildPrintJobRequest(uint8_t* buffer, size_t capacity, size_t& length,
                            const String& printerUri, const String& user,
                            uint32_t jobId, const uint8_t* document,
                            size_t documentLength);
};
