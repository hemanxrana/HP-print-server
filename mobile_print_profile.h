#pragma once

// Android/Mopria-facing IPP profile for a byte-for-byte USB pass-through queue.
// Discovery advertises common mobile PDLs so Android can choose a printable
// representation. The server never parses, validates, converts, or rejects
// the document because of its PDL; the USB printer receives the bytes and
// decides whether it can interpret them.
namespace MobilePrintProfile {
static constexpr const char *SERVICE_TYPE = "_ipp._tcp";
static constexpr const char *SERVICE_SUBTYPE = "_universal._sub._ipp._tcp";
static constexpr uint16_t IPP_PORT = 631;
static constexpr const char *IPP_PATH = "/ipp/print";

// Used for the server's generic default. The actual IPP document-format sent
// by Android is preserved and passed through unchanged.
static constexpr const char *FORMAT_PASSTHROUGH = "application/octet-stream";

static constexpr const char *TXT_VERS = "1.1";
static constexpr const char *TXT_PRODUCT = "(ESP32-S3 USB Print Server)";
static constexpr const char *TXT_NOTE = "USB printer pass-through";

// DNS-SD/Mopria discovery uses this list to decide which documents it can
// submit. This is capability advertisement only; there is no format check in
// the IPP request path.
static constexpr const char *TXT_PDL =
    "application/pdf,application/PCLm,application/vnd.hp-pcl,application/vnd.hp-pclxl,image/pwg-raster,image/urf,image/jpeg,image/png,application/octet-stream";
} // namespace MobilePrintProfile
