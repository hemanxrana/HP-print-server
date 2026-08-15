#include "discovery_engine.h"
#include <ESPmDNS.h>

static const IPAddress SSDP_MULTICAST(239,255,255,250);
static const IPAddress WSD_MULTICAST(239,255,255,250);

PrinterDiscoveryEngine::PrinterDiscoveryEngine() {}
void PrinterDiscoveryEngine::begin() {}
void PrinterDiscoveryEngine::setSNMPTarget(const String &ip) { snmpTarget_ = ip; }

static bool containsIgnoreCase(const String &s, const char *needle) {
  String a = s; a.toLowerCase();
  String b = needle; b.toLowerCase();
  return a.indexOf(b) >= 0;
}

void PrinterDiscoveryEngine::addEndpoint(PrinterEndpoint *results, size_t capacity, size_t &count, const PrinterEndpoint &candidate) {
  if (count >= capacity || candidate.address == IPAddress(0,0,0,0)) return;
  for (size_t i = 0; i < count; ++i) {
    if (results[i].address == candidate.address && results[i].port == candidate.port) return;
  }
  results[count++] = candidate;
}

void PrinterDiscoveryEngine::scanMDNS(PrinterEndpoint *results, size_t capacity, size_t &count) {
  const char *types[] = {"ipp", "ipps", "printer"};
  for (uint8_t t = 0; t < 3; ++t) {
    int n = MDNS.queryService(types[t], "tcp");
    for (int i = 0; i < n && count < capacity; ++i) {
      PrinterEndpoint p{};
      p.name = MDNS.hostname(i);
      p.address = MDNS.IP(i);
      p.port = MDNS.port(i);
      p.url = String(types[t]) + "://" + p.address.toString() + ":" + String(p.port) + "/";
      p.discovery = DISCOVERY_MDNS;
      p.ipp = strcmp(types[t], "ipp") == 0;
      p.ipps = strcmp(types[t], "ipps") == 0;
      addEndpoint(results, capacity, count, p);
    }
  }
}

void PrinterDiscoveryEngine::scanSSDP(PrinterEndpoint *results, size_t capacity, size_t &count, uint32_t timeoutMs) {
  if (!udp_.begin(0)) return;
  const char request[] = "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 1\r\nST: ssdp:all\r\n\r\n";
  udp_.beginPacket(SSDP_MULTICAST, 1900);
  udp_.write((const uint8_t *)request, strlen(request));
  udp_.endPacket();
  unsigned long until = millis() + timeoutMs;
  while ((long)(until - millis()) > 0) {
    int packet = udp_.parsePacket();
    if (packet <= 0) { delay(5); continue; }
    String response;
    while (udp_.available()) response += (char)udp_.read();
    if (!containsIgnoreCase(response, "printer") && !containsIgnoreCase(response, "print")) continue;
    PrinterEndpoint p{};
    p.address = udp_.remoteIP();
    p.port = 80;
    p.discovery = DISCOVERY_SSDP;
    p.url = "http://" + p.address.toString() + "/";
    String lower = response; lower.toLowerCase();
    int loc = lower.indexOf("location:");
    if (loc >= 0) {
      int start = response.indexOf(':', loc) + 1;
      while (start < (int)response.length() && response[start] == ' ') ++start;
      int end = response.indexOf('\n', start);
      p.url = response.substring(start, end < 0 ? response.length() : end);
      p.url.trim();
    }
    p.name = p.url;
    addEndpoint(results, capacity, count, p);
  }
  udp_.stop();
}

void PrinterDiscoveryEngine::scanWSD(PrinterEndpoint *results, size_t capacity, size_t &count, uint32_t timeoutMs) {
  if (!udp_.begin(0)) return;
  const char probe[] = "<?xml version=\"1.0\" encoding=\"UTF-8\"?><e:Envelope xmlns:e=\"http://www.w3.org/2003/05/soap-envelope\" xmlns:w=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\" xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\"><e:Header><w:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe</w:Action><w:MessageID>urn:uuid:00000000-0000-0000-0000-000000000001</w:MessageID><w:To>urn:schemas-xmlsoap-org:ws:2005:04:discovery</w:To></e:Header><e:Body><d:Probe><d:Types>dpws:Device</d:Types></d:Probe></e:Body></e:Envelope>";
  udp_.beginPacket(WSD_MULTICAST, 3702);
  udp_.write((const uint8_t *)probe, strlen(probe));
  udp_.endPacket();
  unsigned long until = millis() + timeoutMs;
  while ((long)(until - millis()) > 0) {
    int packet = udp_.parsePacket();
    if (packet <= 0) { delay(5); continue; }
    String response;
    while (udp_.available()) response += (char)udp_.read();
    if (!containsIgnoreCase(response, "print") && !containsIgnoreCase(response, "printer")) continue;
    PrinterEndpoint p{};
    p.address = udp_.remoteIP();
    p.port = 5357;
    p.discovery = DISCOVERY_WSD;
    p.wsd = true;
    p.name = "WSD printer";
    p.url = "http://" + p.address.toString() + ":5357/";
    addEndpoint(results, capacity, count, p);
  }
  udp_.stop();
}

void PrinterDiscoveryEngine::scanSNMP(PrinterEndpoint *results, size_t capacity, size_t &count, uint32_t timeoutMs) {
  if (snmpTarget_.isEmpty() || !udp_.begin(0)) return;
  IPAddress ip;
  if (!ip.fromString(snmpTarget_)) { udp_.stop(); return; }
  const uint8_t req[] = {0x30,0x26,0x02,0x01,0x00,0x04,0x06,'p','u','b','l','i','c',0xA0,0x19,0x02,0x01,0x01,0x02,0x01,0x00,0x02,0x01,0x00,0x30,0x0E,0x30,0x0C,0x06,0x08,0x2B,0x06,0x01,0x02,0x01,0x01,0x01,0x00,0x05,0x00};
  udp_.beginPacket(ip, 161); udp_.write(req, sizeof(req)); udp_.endPacket();
  unsigned long until = millis() + timeoutMs;
  while ((long)(until - millis()) > 0) {
    int packet = udp_.parsePacket();
    if (packet <= 0) { delay(5); continue; }
    uint8_t buffer[512]; int len = 0;
    while (udp_.available() && len < (int)sizeof(buffer)) buffer[len++] = udp_.read();
    if (len < 16) continue;
    PrinterEndpoint p{}; p.address = udp_.remoteIP(); p.port = 161; p.discovery = DISCOVERY_SNMP; p.name = "SNMP printer candidate"; p.url = "snmp://" + p.address.toString();
    addEndpoint(results, capacity, count, p); break;
  }
  udp_.stop();
}

size_t PrinterDiscoveryEngine::scan(PrinterEndpoint *results, size_t capacity, uint32_t timeoutMs) {
  if (!results || capacity == 0 || WiFi.status() != WL_CONNECTED) return 0;
  size_t count = 0;
  scanMDNS(results, capacity, count);
  scanSSDP(results, capacity, count, timeoutMs);
  scanWSD(results, capacity, count, timeoutMs);
  scanSNMP(results, capacity, count, timeoutMs);
  return count;
}
