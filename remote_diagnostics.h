#pragma once

#include <Arduino.h>

// BLE UART-style diagnostic console for ESP32-S3 using NimBLE-Arduino.  The S3
// supports BLE, not Bluetooth Classic SPP, so this is intentionally a GATT
// console rather than BluetoothSerial.  NimBLE keeps the image inside the
// default 1.25 MB app partition.
namespace RemoteDiagnostics {
using CommandHook = void (*)(const String &command);
void begin();
void poll();
bool connected();
void publish(const String &message);
void publishf(const char *format, ...);
void setCommandHook(CommandHook hook);
String recentLog();
const char *logData(size_t &length);
bool logInPsram();
}
