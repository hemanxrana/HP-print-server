#pragma once

// SMART_TANK_540 compatibility profile for the network/mobile-print side.
//
// The real 540-family is a Wi-Fi MFP with HP PCL 3 GUI, PCLm and URF
// printing, plus AirPrint/Mopria/HP Android mobile printing. Public HP-family
// captures show the same Bonjour/IPPS vocabulary used here. The ESP32 does
// NOT render or convert documents: the selected PDL is passed unchanged to
// the USB printer backend.
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

// PCLm is the safest default for the HP mobile-print path. URF is advertised
// as the AirPrint format. PCL 3 GUI is a printer language/driver family, not
// an IPP document-format MIME type, so it is represented in device-id/TXT
// metadata rather than pretending it is an IPP MIME type.
static constexpr const char *FORMAT_PASSTHROUGH = "application/PCLm";
static constexpr const char *FORMAT_PCLM = "application/PCLm";
static constexpr const char *FORMAT_URF = "image/urf";
static constexpr const char *FORMAT_PCL = "application/vnd.hp-PCL";
static constexpr const char *FORMAT_JPEG = "image/jpeg";
static constexpr const char *FORMAT_PWG = "image/pwg-raster";
static constexpr const char *FORMAT_OCTET = "application/octet-stream";

// Bonjour/AirPrint TXT profile. The URF value follows the verified HP Smart
// Tank-family advertisement pattern (including V1.5 and the 300/600 dpi
// raster modes); it is discovery metadata, not a claim that the ESP32 renders
// URF itself.
static constexpr const char *TXT_VERS = "1";
static constexpr const char *TXT_PRODUCT = "(HP Smart Tank 540 series)";
static constexpr const char *TXT_NOTE = "";
static constexpr const char *TXT_TY = "HP Smart Tank 540 series";
static constexpr const char *TXT_PDL = "application/PCLm,image/urf";
static constexpr const char *TXT_URF =
    "CP1,MT1-2-8-9-10-11,PQ3-4-5,RS300-600,SRGB24,OB9,OFU0,W8-16,DEVW8-16,DEVRGB24-48,ADOBERGB24-48,FN3,IS1,V1.5";
static constexpr const char *TXT_KIND = "document,envelope,photo,postcard";
static constexpr const char *TXT_PAPER_MAX = "legal-A4";
static constexpr const char *TXT_USB_MFG = "HP";
static constexpr const char *TXT_USB_MDL = "Smart Tank 540 series";
static constexpr const char *TXT_USB_CMD = "PCL3GUI,PJL,PCLM,URF";
static constexpr const char *TXT_MOPRIA = "2.0";
static constexpr const char *TXT_AIR = "none";
static constexpr const char *TXT_COLOR = "T";
static constexpr const char *TXT_DUPLEX = "F"; // 540-family manual/two-sided workflow
static constexpr const char *TXT_FAX = "F";
static constexpr const char *TXT_SCAN = "T";
static constexpr const char *TXT_PRIORITY = "20";
static constexpr const char *TXT_QTOTAL = "1";

} // namespace MobilePrintProfile
