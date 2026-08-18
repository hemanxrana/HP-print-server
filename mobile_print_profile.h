#pragma once

// Single-format mobile printing profile.
// The ESP32 advertises and accepts only HP PCL 3 GUI / HP PCL.
namespace MobilePrintProfile {
static constexpr const char *SERVICE_TYPE = "_ipp._tcp";
static constexpr const char *SERVICE_SUBTYPE = "_universal._sub._ipp._tcp";
static constexpr uint16_t IPP_PORT = 631;
static constexpr const char *IPP_PATH = "/ipp/print";

// MIME media type used by the IPP endpoint and DNS-SD pdl attribute.
static constexpr const char *FORMAT_PCL3GUI = "application/vnd.hp-pcl";
// Accepted as an input alias only; never advertised to clients.
static constexpr const char *FORMAT_PCL3GUI_ALIAS = "application/vnd.hp-pcl3gui";

static constexpr const char *TXT_VERS = "1.1";
static constexpr const char *TXT_PRODUCT = "(ESP32-S3 HP Print Server - HP PCL 3 GUI)";
static constexpr const char *TXT_NOTE = "HP PCL 3 GUI only";
static constexpr const char *TXT_PDL = "application/vnd.hp-pcl";
} // namespace MobilePrintProfile
