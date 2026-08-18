#pragma once
#include <Arduino.h>

// Discovery and transport are intentionally independent. A printer can be
// discovered with mDNS, WSD, SSDP, SNMP, or a direct/manual address and still
// accept jobs over a different transport.

enum PrinterTransport : uint8_t {
  TRANSPORT_USB = 0,
  TRANSPORT_IPP,
  TRANSPORT_IPPS,
  TRANSPORT_JETDIRECT_RAW,
  TRANSPORT_LPR,
  TRANSPORT_WSD,
  TRANSPORT_HTTP,
  TRANSPORT_HTTPS
};

enum PrinterDiscovery : uint8_t {
  DISCOVERY_MANUAL_IP = 0,
  DISCOVERY_MDNS,
  DISCOVERY_SSDP,
  DISCOVERY_WSD,
  DISCOVERY_SNMP,
  DISCOVERY_HTTP_PROBE,
  DISCOVERY_WIFI_DIRECT
};

constexpr uint8_t PRINTER_TRANSPORT_FIRST = TRANSPORT_USB;
constexpr uint8_t PRINTER_TRANSPORT_LAST = TRANSPORT_HTTPS;
constexpr uint8_t PRINTER_DISCOVERY_FIRST = DISCOVERY_MANUAL_IP;
constexpr uint8_t PRINTER_DISCOVERY_LAST = DISCOVERY_WIFI_DIRECT;

inline const char *transportName(PrinterTransport transport) {
  switch (transport) {
    case TRANSPORT_USB: return "USB Host";
    case TRANSPORT_IPP: return "IPP";
    case TRANSPORT_IPPS: return "IPPS";
    case TRANSPORT_JETDIRECT_RAW: return "JetDirect / RAW 9100";
    case TRANSPORT_LPR: return "LPR/LPD 515";
    case TRANSPORT_WSD: return "WSD";
    case TRANSPORT_HTTP: return "HTTP";
    case TRANSPORT_HTTPS: return "HTTPS";
    default: return "Unknown";
  }
}

inline const char *discoveryName(PrinterDiscovery discovery) {
  switch (discovery) {
    case DISCOVERY_MANUAL_IP: return "Manual IP";
    case DISCOVERY_MDNS: return "mDNS / DNS-SD";
    case DISCOVERY_SSDP: return "SSDP / UPnP";
    case DISCOVERY_WSD: return "WS-Discovery";
    case DISCOVERY_SNMP: return "SNMP";
    case DISCOVERY_HTTP_PROBE: return "HTTP printer probe";
    case DISCOVERY_WIFI_DIRECT: return "Wi-Fi Direct";
    default: return "Unknown";
  }
}

inline bool isValidTransport(uint8_t value) {
  return value >= PRINTER_TRANSPORT_FIRST && value <= PRINTER_TRANSPORT_LAST;
}

inline bool isValidDiscovery(uint8_t value) {
  return value >= PRINTER_DISCOVERY_FIRST && value <= PRINTER_DISCOVERY_LAST;
}
