#pragma once

#include <Arduino.h>

// Print-compatible logger that mirrors every write to the UART and to the
// in-memory diagnostic ring.  The ring is what /log, /monitor and the TCP log
// stream serve, so the network monitor sees the same text as the serial port.
class Logger : public Print {
public:
  void begin(unsigned long baud);
  size_t write(uint8_t c) override;
  size_t write(const uint8_t *buffer, size_t size) override;
};

extern Logger Log;
