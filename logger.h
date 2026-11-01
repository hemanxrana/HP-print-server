#pragma once

#include <Arduino.h>

// Print-compatible logger that writes every message to the UART.
// It intentionally takes the place of direct Serial calls so all
// diagnostics flow through a single chokepoint.
class Logger : public Print {
public:
  void begin(unsigned long baud);
  size_t write(uint8_t c) override;
  size_t write(const uint8_t *buffer, size_t size) override;
};

extern Logger Log;