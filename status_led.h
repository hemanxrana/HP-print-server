#pragma once
#include <Arduino.h>

namespace StatusLed {
  enum State : uint8_t {
    BOOT,
    WIFI_CONNECTING,
    WIFI_CONNECTED,
    WAITING_FOR_PRINTER,
    PRINTER_READY,
    PRINTING,
    ERROR
  };

  void begin();
  void set(State state);
  void update();
}
