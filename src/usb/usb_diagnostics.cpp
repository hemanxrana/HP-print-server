#include "usb_diagnostics.h"

namespace UsbDiagnostics {
void printDevice(const UsbDeviceInfo &d, Stream &out) {
  out.printf("[USB] Device attached: %s\n", d.attached ? "yes" : "no");
  out.printf("[USB] VID: 0x%04X  PID: 0x%04X  address: %u\n", d.vid, d.pid, d.address);
  out.printf("[USB] Configuration: %u\n", d.configurationValue);
  if (d.manufacturer.length()) out.printf("[USB] Manufacturer: %s\n", d.manufacturer.c_str());
  if (d.product.length()) out.printf("[USB] Product: %s\n", d.product.c_str());
  if (d.serial.length()) out.printf("[USB] Serial: %s\n", d.serial.c_str());
  if (!d.printer.found) {
    out.println("[USB] Printer Class interface: not found");
    return;
  }
  out.printf("[USB] Printer interface: %u alt=%u subclass=0x%02X protocol=0x%02X\n",
             d.printer.interfaceNumber, d.printer.alternateSetting,
             d.printer.subclass, d.printer.protocol);
  if (d.printer.bulkOut.valid()) {
    out.printf("[USB] Bulk OUT: 0x%02X maxPacket=%u\n",
               d.printer.bulkOut.address, d.printer.bulkOut.maxPacketSize);
  }
  if (d.printer.bulkIn.valid()) {
    out.printf("[USB] Bulk IN: 0x%02X maxPacket=%u\n",
               d.printer.bulkIn.address, d.printer.bulkIn.maxPacketSize);
  }
}
}
