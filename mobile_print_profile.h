#pragma once

// Mobile-first IPP profile. Android's built-in Default Print Service is
// powered by Mopria technology; IPP Everywhere uses DNS-SD and standard
// mobile-friendly document formats.
namespace MobilePrintProfile {
static constexpr const char *SERVICE_TYPE = "_ipp._tcp";
static constexpr const char *SERVICE_SUBTYPE = "_universal._sub._ipp._tcp";
static constexpr uint16_t IPP_PORT = 631;
static constexpr const char *IPP_PATH = "/ipp/print";
static constexpr const char *FORMAT_PWG = "image/pwg-raster";
static constexpr const char *FORMAT_PCLM = "application/PCLm";
static constexpr const char *FORMAT_PDF = "application/pdf";
static constexpr const char *FORMAT_JPEG = "image/jpeg";
static constexpr const char *FORMAT_URF = "image/urf";
static constexpr const char *TXT_VERS = "1.1";
static constexpr const char *TXT_PRODUCT = "(ESP32-S3 HP Print Server)";
static constexpr const char *TXT_NOTE = "USB print server for HP printers";
} // namespace MobilePrintProfile
