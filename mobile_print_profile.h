#pragma once

// Compatibility profile for the Wi-Fi/mobile-printing side of the
// HP Smart Tank 540 family. This is intentionally independent from the
// USB pass-through implementation: the ESP32 still forwards the selected
// printer PDL unchanged and does not render documents.
namespace MobilePrintProfile {

static constexpr const char *PROFILE_NAME = "SMART_TANK_540";
static constexpr const char *MODEL = "HP Smart Tank 540 series";
static constexpr const char *SERVICE_TYPE = "_ipp._tcp";
static constexpr const char *SERVICE_SUBTYPE = "_print._sub._ipp._tcp";
static constexpr uint16_t IPP_PORT = 631;
static constexpr const char *IPP_PATH = "/ipp/print";

// The public 540-family specifications identify PCLm and URF as printer
// languages, while public HP-family Bonjour captures show the same mobile
// print stack advertising PCL, JPEG, PCLm, URF and PWG Raster. These are
// advertised as pass-through formats only; the ESP32 does not convert them.
static constexpr const char *FORMAT_PASSTHROUGH = "application/PCLm";
static constexpr const char *TXT_VERS = "1";
static constexpr const char *TXT_PRODUCT = "(HP Smart Tank 540 series)";
static constexpr const char *TXT_NOTE = "";
static constexpr const char *TXT_TY = "HP Smart Tank 540 series";
static constexpr const char *TXT_PDL =
    "application/vnd.hp-PCL,image/jpeg,application/PCLm,image/urf,image/pwg-raster,application/octet-stream";
static constexpr const char *TXT_URF =
    "CP1,MT1-2-8-9-10-11,PQ3-4-5,RS300-600,SRGB24,OB9,OFU0,W8-16,DEVW8-16,DEVRGB24-48,ADOBERGB24-48,FN3,IS1,V1.5";
static constexpr const char *TXT_KIND = "document,envelope,photo,postcard";
static constexpr const char *TXT_PAPER_MAX = "legal-A4";
static constexpr const char *TXT_USB_MFG = "HP";
static constexpr const char *TXT_USB_MDL = "Smart Tank 540 series";
static constexpr const char *TXT_MOPRIA = "2.0";
static constexpr const char *TXT_AIR = "none";
static constexpr const char *TXT_COLOR = "T";
static constexpr const char *TXT_DUPLEX = "F";
static constexpr const char *TXT_FAX = "F";
static constexpr const char *TXT_SCAN = "T";
static constexpr const char *TXT_PRIORITY = "20";
static constexpr const char *TXT_QTOTAL = "1";

// IPP document formats exposed by the compatibility profile. The phone or
// computer chooses one of these; the ESP32 forwards the bytes unchanged.
static constexpr const char *FORMAT_PCL = "application/vnd.hp-PCL";
static constexpr const char *FORMAT_JPEG = "image/jpeg";
static constexpr const char *FORMAT_URF = "image/urf";
static constexpr const char *FORMAT_PWG = "image/pwg-raster";
static constexpr const char *FORMAT_OCTET = "application/octet-stream";

} // namespace MobilePrintProfile
