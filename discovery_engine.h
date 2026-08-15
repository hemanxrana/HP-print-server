#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include "printer_protocols.h"
struct PrinterEndpoint{String name;String model;IPAddress address;String hostname;String url;PrinterDiscovery discovery;uint16_t port;bool ipp;bool ipps;bool raw9100;bool lpr;bool wsd;};
class PrinterDiscoveryEngine{public:PrinterDiscoveryEngine();void begin();void setSNMPTarget(const String&ip);size_t scan(PrinterEndpoint*results,size_t capacity,uint32_t timeoutMs=1500);private:WiFiUDP udp_;String snmpTarget_;void addEndpoint(PrinterEndpoint*results,size_t capacity,size_t&count,const PrinterEndpoint&candidate);void scanSSDP(PrinterEndpoint*results,size_t capacity,size_t&count,uint32_t timeoutMs);void scanWSD(PrinterEndpoint*results,size_t capacity,size_t&count,uint32_t timeoutMs);void scanSNMP(PrinterEndpoint*results,size_t capacity,size_t&count,uint32_t timeoutMs);void scanMDNS(PrinterEndpoint*results,size_t capacity,size_t&count);};
