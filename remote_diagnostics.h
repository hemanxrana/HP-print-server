#pragma once

#include <Arduino.h>

// In-memory RAM log ring exposed through the HTTP /log endpoint.  The BLE GATT
// console was removed so the firmware fits the default 1.25 MB app partition;
// all diagnostics remain available over HTTP and the USB serial console.
namespace RemoteDiagnostics {
void begin();
void publish(const String &message);
void publishf(const char *format, ...);
String recentLog();
}
