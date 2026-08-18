#pragma once
#include <Arduino.h>

// This project intentionally exposes only the transports it actually uses.
// LPR/LPD and the unused legacy transport enums have been removed.
enum PrinterTransport : uint8_t {
  TRANSPORT_USB = 0,
  TRANSPORT_IPP,
  TRANSPORT_JETDIRECT_RAW
};

enum PrinterDiscovery : uint8_t {
  DISCOVERY_MANUAL_IP = 0,
  DISCOVERY_MDNS
};

constexpr uint8_t PRINTER_TRANSPORT_FIRST = TRANSPORT_USB;
constexpr uint8_t PRINTER_TRANSPORT_LAST = TRANSPORT_JETDIRECT_RAW;
constexpr uint8_t PRINTER_DISCOVERY_FIRST = DISCOVERY_MANUAL_IP;
constexpr uint8_t PRINTER_DISCOVERY_LAST = DISCOVERY_MDNS;

inline const char *transportName(PrinterTransport transport) {
  switch (transport) {
    case TRANSPORT_USB: return "USB Host";
    case TRANSPORT_IPP: return "IPP";
    case TRANSPORT_JETDIRECT_RAW: return "JetDirect / RAW 9100";
    default: return "Unknown";
  }
}

inline const char *discoveryName(PrinterDiscovery discovery) {
  switch (discovery) {
    case DISCOVERY_MANUAL_IP: return "Manual IP";
    case DISCOVERY_MDNS: return "mDNS / DNS-SD";
    default: return "Unknown";
  }
}

inline bool isValidTransport(uint8_t value) {
  return value >= PRINTER_TRANSPORT_FIRST && value <= PRINTER_TRANSPORT_LAST;
}

inline bool isValidDiscovery(uint8_t value) {
  return value >= PRINTER_DISCOVERY_FIRST && value <= PRINTER_DISCOVERY_LAST;
}
