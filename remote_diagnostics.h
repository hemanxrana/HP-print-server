#pragma once

#include <Arduino.h>

// BLE UART-style diagnostic console for ESP32-S3 using NimBLE-Arduino.  The S3
// supports BLE, not Bluetooth Classic SPP, so this is intentionally a GATT
// console rather than BluetoothSerial.  NimBLE keeps the image inside the
// default 1.25 MB app partition.
namespace RemoteDiagnostics {
using CommandHook = void (*)(const String &command);

// Initialise the in-memory log ring.  BLE is started separately with
// configure() so the saved dashboard settings can be applied first.
void begin();
void configure(bool enabled, const String &deviceName);
bool enabled();
bool ready();
bool connected();
String deviceName();

void poll();
void publish(const String &message);
void publishf(const char *format, ...);
void setCommandHook(CommandHook hook);

// Raw append used by the Logger tee.  Keeps UART output in the same ring that
// the web/TCP monitor reads.
void appendRaw(const char *data, size_t length);

String recentLog();
const char *logData(size_t &length);
uint32_t logSequence();
bool logInPsram();
}
