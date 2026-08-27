#pragma once
#include <Arduino.h>

class UsbScannerBackend {
public:
  static constexpr uint16_t NETWORK_PORT = 8080;

  bool begin();
  bool ready() const;
  bool busy() const;
  uint8_t interfaceNumber() const;
  uint8_t alternateSetting() const;
  uint8_t bulkOutEndpoint() const;
  uint8_t bulkInEndpoint() const;

private:
  bool started_ = false;
};
