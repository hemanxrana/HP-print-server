#include "discovery_engine.h"
#include <ESPmDNS.h>
#include <string.h>

PrinterDiscoveryEngine::PrinterDiscoveryEngine() {}

void PrinterDiscoveryEngine::begin() {}

void PrinterDiscoveryEngine::setSNMPTarget(const String &ip) {
  // Retained for source compatibility. SNMP discovery is intentionally disabled.
  snmpTarget_ = ip;
}

void PrinterDiscoveryEngine::addEndpoint(PrinterEndpoint *results,
                                         size_t capacity,
                                         size_t &count,
                                         const PrinterEndpoint &candidate) {
  if (count >= capacity || candidate.address == IPAddress(0, 0, 0, 0)) return;

  for (size_t i = 0; i < count; ++i) {
    if (results[i].address == candidate.address &&
        results[i].port == candidate.port &&
        results[i].discovery == candidate.discovery) {
      return;
    }
  }

  results[count++] = candidate;
}

void PrinterDiscoveryEngine::scanMDNS(PrinterEndpoint *results,
                                      size_t capacity,
                                      size_t &count) {
  // DNS-SD/mDNS is the only automatic network discovery mechanism exposed by
  // this project. Keep the advertised services limited to printer transports
  // that the firmware actually implements.
  const char *types[] = {"ipp", "ipps", "printer"};

  for (uint8_t t = 0; t < 3 && count < capacity; ++t) {
    int n = MDNS.queryService(types[t], "tcp");
    for (int i = 0; i < n && count < capacity; ++i) {
      PrinterEndpoint p{};
      p.name = MDNS.hostname(i);
      p.address = MDNS.address(i);
      p.port = MDNS.port(i);
      p.url = String(types[t]) + "://" + p.address.toString() + ":" + String(p.port) + "/";
      p.discovery = DISCOVERY_MDNS;
      p.ipp = strcmp(types[t], "ipp") == 0;
      p.ipps = strcmp(types[t], "ipps") == 0;
      p.raw9100 = false;
      p.lpr = false;
      p.wsd = false;
      addEndpoint(results, capacity, count, p);
    }
  }
}

size_t PrinterDiscoveryEngine::scan(PrinterEndpoint *results,
                                    size_t capacity,
                                    uint32_t /*timeoutMs*/) {
  if (!results || capacity == 0 || WiFi.status() != WL_CONNECTED) return 0;

  size_t count = 0;
  scanMDNS(results, capacity, count);
  return count;
}
