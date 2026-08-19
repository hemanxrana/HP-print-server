#pragma once

// Android/Mopria-facing IPP profile for a byte-for-byte USB pass-through
// printer. The ESP32 does NOT render PDF/JPEG/Office documents. The mobile
// print service is responsible for producing one of the printer-oriented PDLs
// below; the ESP32 forwards the resulting document unchanged to USB.
namespace MobilePrintProfile {
static constexpr const char *SERVICE_TYPE = "_ipp._tcp";
static constexpr const char *SERVICE_SUBTYPE = "_print._sub._ipp._tcp";
static constexpr uint16_t IPP_PORT = 631;
static constexpr const char *IPP_PATH = "/ipp/print";

// PCLm is the preferred mobile-print representation for this HP family.
// PWG Raster is retained as a standards-based alternative used by Android/Mopria.
// This default is used when an IPP client omits document-format.
static constexpr const char *FORMAT_PASSTHROUGH = "application/PCLm";

static constexpr const char *TXT_VERS = "1.1";
static constexpr const char *TXT_PRODUCT = "(ESP32-S3 USB Print Server)";
static constexpr const char *TXT_NOTE = "USB printer pass-through";

// Keep discovery conservative: advertise formats the phone-side print
// subsystem can render for mobile printing and that are suitable for direct
// forwarding to the printer. Do not advertise Office/PDF/image formats as if
// the ESP32 itself could interpret them.
static constexpr const char *TXT_PDL =
    "application/PCLm,image/pwg-raster";
} // namespace MobilePrintProfile
