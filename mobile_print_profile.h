#pragma once

// SMART_TANK_540 compatibility profile for the network/mobile-print side.
// The network identity follows the same HP Smart Tank Wi-Fi/IPP vocabulary
// observed in public HP-family Bonjour captures. USB pass-through is unchanged.
namespace MobilePrintProfile {

static constexpr const char *PROFILE_NAME = "SMART_TANK_540";
static constexpr const char *DISPLAY_NAME = "HP Smart Tank 540 series";
static constexpr const char *MODEL = "HP Smart Tank 540 series";
static constexpr const char *HOSTNAME = "hp-smart-tank-540";
static constexpr const char *AP_SSID = "HP Smart Tank 540";
static constexpr const char *SERVICE_TYPE = "_ipp._tcp";
static constexpr const char *SERVICE_SUBTYPE = "_print._sub._ipp._tcp";
static constexpr uint16_t IPP_PORT = 631;
static constexpr const char *IPP_PATH = "/ipp/print";

// The 580/590 and 5100 families document PCLm and URF as their mobile/AirPrint
// print languages. We therefore keep these as the IPP document formats that
// the ESP32 advertises and forwards unchanged to USB.
static constexpr const char *FORMAT_PASSTHROUGH = "application/PCLm";
static constexpr const char *FORMAT_PCLM = "application/PCLm";
static constexpr const char *FORMAT_URF = "image/urf";
static constexpr const char *FORMAT_PCL = "application/vnd.hp-PCL";
static constexpr const char *FORMAT_JPEG = "image/jpeg";
static constexpr const char *FORMAT_PWG = "image/pwg-raster";
static constexpr const char *FORMAT_OCTET = "application/octet-stream";

// Verified HP Smart Tank-family Bonjour/IPPs TXT vocabulary. A public
// Smart Tank 5100 capture exposes these values, while HP's 580 specification
// independently confirms PCLm + URF, Wi-Fi, AirPrint, Mopria and HP Android
// printing. These are discovery hints only; no rendering/conversion occurs.
static constexpr const char *TXT_VERS = "1";
static constexpr const char *TXT_PRODUCT = "(HP Smart Tank 540 series)";
static constexpr const char *TXT_NOTE = "";
static constexpr const char *TXT_TY = "HP Smart Tank 540 series";
static constexpr const char *TXT_PDL = "application/vnd.hp-PCL,image/jpeg,application/PCLm,image/urf,image/pwg-raster,application/octet-stream";
static constexpr const char *TXT_URF =
    "CP1,MT1-2-8-9-10-11,PQ3-4-5,RS300-600,SRGB24,OB9,OFU0,W8-16,DEVW8-16,DEVRGB24-48,ADOBERGB24-48,FN3,IS1,V1.5";
static constexpr const char *TXT_KIND = "document,envelope,photo,postcard";
static constexpr const char *TXT_PAPER_MAX = "legal-A4";
static constexpr const char *TXT_USB_MFG = "HP";
static constexpr const char *TXT_USB_MDL = "Smart Tank 540 series";
static constexpr const char *TXT_USB_CMD = "PCL3GUI,PJL,Automatic,JPEG,PCLM,AppleRaster,PWGRaster,DW-PCL,802.11,DESKJET,DYN";
static constexpr const char *TXT_MOPRIA = "2.1";
static constexpr const char *TXT_AIR = "none";
static constexpr const char *TXT_COLOR = "T";
static constexpr const char *TXT_DUPLEX = "F";
static constexpr const char *TXT_FAX = "F";
static constexpr const char *TXT_SCAN = "T";
static constexpr const char *TXT_PRIORITY = "50";
static constexpr const char *TXT_QTOTAL = "1";

} // namespace MobilePrintProfile
