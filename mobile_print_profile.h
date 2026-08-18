#pragma once

// Pass-through mobile printing profile.
// The server does not validate or convert the document format. The selected
// USB printer interface receives the submitted bytes and the printer decides
// whether it can interpret them.
namespace MobilePrintProfile {
static constexpr const char *SERVICE_TYPE = "_ipp._tcp";
static constexpr const char *SERVICE_SUBTYPE = "_universal._sub._ipp._tcp";
static constexpr uint16_t IPP_PORT = 631;
static constexpr const char *IPP_PATH = "/ipp/print";

// Generic pass-through MIME. It is not used as a format restriction.
static constexpr const char *FORMAT_PASSTHROUGH = "application/octet-stream";

static constexpr const char *TXT_VERS = "1.1";
static constexpr const char *TXT_PRODUCT = "(ESP32-S3 HP Print Server - pass-through)";
static constexpr const char *TXT_NOTE = "USB printer pass-through";
static constexpr const char *TXT_PDL = "application/octet-stream";
} // namespace MobilePrintProfile
