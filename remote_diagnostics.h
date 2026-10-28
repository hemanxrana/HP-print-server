#pragma once

#include <Arduino.h>

// BLE UART-style diagnostic console for ESP32-S3.  The S3 supports BLE, not
// Bluetooth Classic SPP, so this is intentionally a GATT console rather than
// BluetoothSerial.
namespace RemoteDiagnostics {
void begin();
void poll();
bool connected();
void publish(const String &message);
void publishf(const char *format, ...);
}
