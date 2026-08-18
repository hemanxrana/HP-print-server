#pragma once
#include <Arduino.h>
#include "usb_device.h"

namespace UsbDiagnostics {
void printDevice(const UsbDeviceInfo &device, Stream &out = Serial);
}
