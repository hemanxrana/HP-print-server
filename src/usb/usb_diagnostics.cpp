#include "usb_diagnostics.h"

namespace UsbDiagnostics {
void printDevice(const UsbDeviceInfo &d, Stream &out) {
  out.printf("[USB] Device attached: %s\n", d.attached ? "yes" : "no");
  out.printf("[USB] VID: 0x%04X  PID: 0x%04X  address: %u\n", d.vid, d.pid, d.address);
  out.printf("[USB] Configuration: %u\n", d.configurationValue);
  if (d.manufacturer.length()) out.printf("[USB] Manufacturer: %s\n", d.manufacturer.c_str());
  if (d.product.length()) out.printf("[USB] Product: %s\n", d.product.c_str());
  if (d.serial.length()) out.printf("[USB] Serial: %s\n", d.serial.c_str());

  if (!d.printerInterfaceCount) {
    out.println("[USB] Printer Class interfaces: none");
    return;
  }

  out.printf("[USB] Printer Class candidates: %u\n", d.printerInterfaceCount);
  for (uint8_t i = 0; i < d.printerInterfaceCount; ++i) {
    const UsbPrinterInterfaceInfo &p = d.printerInterfaces[i];
    out.printf("  #%u IF=%u ALT=%u subclass=0x%02X protocol=0x%02X OUT=0x%02X IN=0x%02X%s\n",
               i, p.interfaceNumber, p.alternateSetting, p.subclass, p.protocol,
               p.bulkOut.address, p.bulkIn.address,
               (p.interfaceNumber == d.printer.interfaceNumber && p.alternateSetting == d.printer.alternateSetting) ? " [ACTIVE]" : "");
  }
}
}
