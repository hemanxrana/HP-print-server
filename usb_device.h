#pragma once
#include <Arduino.h>

struct UsbEndpointInfo {
  uint8_t address = 0;
  uint8_t attributes = 0;
  uint16_t maxPacketSize = 0;
  uint8_t interval = 0;

  bool valid() const { return address != 0 && maxPacketSize != 0; }
  bool isIn() const { return (address & 0x80) != 0; }
  bool isBulk() const { return (attributes & 0x03) == 0x02; }
};

struct UsbPrinterInterfaceInfo {
  bool found = false;
  uint8_t interfaceNumber = 0;
  uint8_t alternateSetting = 0;
  uint8_t subclass = 0;
  uint8_t protocol = 0;
  UsbEndpointInfo bulkOut;
  UsbEndpointInfo bulkIn;

  bool usableForRawPrint() const {
    return found && bulkOut.valid() && bulkOut.isBulk() && !bulkOut.isIn();
  }

  bool usableForStatus() const {
    return found && subclass == 0x01;
  }
};

struct UsbInterfaceInfo {
  bool found = false;
  uint8_t interfaceNumber = 0;
  uint8_t alternateSetting = 0;
  uint8_t classCode = 0;
  uint8_t subclass = 0;
  uint8_t protocol = 0;
  UsbEndpointInfo bulkOut;
  UsbEndpointInfo bulkIn;
  UsbEndpointInfo interruptOut;
  UsbEndpointInfo interruptIn;

  bool isScanner() const {
    return found && classCode == 0x06 && subclass == 0x01 && protocol == 0x01;
  }
};

struct UsbPortStatus {
  bool valid = false;
  uint8_t value = 0;
  bool error = false;
  bool selected = false;
  bool paperEmpty = false;
  unsigned long updatedAt = 0;
};

struct UsbDeviceInfo {
  static constexpr uint8_t MAX_PRINTER_INTERFACES = 8;
  static constexpr uint8_t MAX_INTERFACES = 16;

  bool attached = false;
  uint16_t vid = 0;
  uint16_t pid = 0;
  uint8_t address = 0;
  uint8_t configurationValue = 0;
  String manufacturer;
  String product;
  String serial;

  uint8_t interfaceCount = 0;
  UsbInterfaceInfo interfaces[MAX_INTERFACES];

  uint8_t printerInterfaceCount = 0;
  UsbPrinterInterfaceInfo printerInterfaces[MAX_PRINTER_INTERFACES];

  // Interface used for RAW Bulk OUT printing.
  UsbPrinterInterfaceInfo printer;

  // Independent Printer Class interface used for GET_PORT_STATUS when the
  // descriptor exposes a second suitable interface.
  UsbPrinterInterfaceInfo statusInterface;
  bool statusInterfaceSeparate = false;
  UsbPortStatus portStatus;
};
