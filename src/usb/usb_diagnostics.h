#pragma once
#include <Arduino.h>
#include "usb_device.h"

namespace UsbDiagnostics {
void printDevice(const UsbDeviceInfo &d, Stream &out = Serial);
}
