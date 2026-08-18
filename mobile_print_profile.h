#pragma once

// Mobile IPP profile deliberately exposes only the native HP PCL 3 GUI
// document language used by the Smart Tank 520/540 USB printer path.
namespace MobilePrintProfile {
static constexpr const char *SERVICE_TYPE = "_ipp._tcp";
static constexpr const char *SERVICE_SUBTYPE = "_universal._sub._ipp._tcp";
static constexpr uint16_t IPP_PORT = 631;
static constexpr const char *IPP_PATH = "/ipp/print";
static constexpr const char *FORMAT_PCL3GUI = "application/vnd.hp-PCL";
static constexpr const char *TXT_VERS = "1.1";
static constexpr const char *TXT_PRODUCT = "(ESP32-S3 HP Print Server - HP PCL 3 GUI)";
static constexpr const char *TXT_NOTE = "USB print server for HP PCL 3 GUI printers";
} // namespace MobilePrintProfile
