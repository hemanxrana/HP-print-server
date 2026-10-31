#ifndef CONFIG_H
#define CONFIG_H

// ============================================================
// ESP32-S3 HP Print Server - Configuration
// Target printer: HP Smart Tank 520
// Board: ESP32-S3-N16R8
// ============================================================

// --- WiFi Default Settings ---
// First boot enters AP mode for setup; these are fallback defaults
#define DEFAULT_WIFI_SSID     ""
#define DEFAULT_WIFI_PASSWORD ""

// --- AP Mode Settings (for initial setup) ---
#define AP_SSID       "HP-Print-Server"
#define AP_PASSWORD   "configureme"

// --- Print Server Settings ---
#define PRINTER_NAME       "HP_SmartTank520_WiFi"
#define RAW_SOCKET_PORT    9100    // JetDirect/AppSocket protocol port
#define IPP_PORT           631     // IPP protocol port (PCL3GUI only)
#define WEB_SERVER_PORT    80      // Web management interface port
#define MDNS_HOSTNAME      "printer"  // Accessible via printer.local
#define STATIC_IP          IPAddress(192, 168, 1, 111)
#define STATIC_GW          IPAddress(192, 168, 1, 1)
#define STATIC_SUBNET      IPAddress(255, 255, 255, 0)

// --- USB Bulk Out Settings ---
#define BULK_OUT_CHUNK_SIZE       (8 * 1024)    // 8KB chunks, no MPS alignment
#define PSRAM_RING_BUFFER_SIZE    (32 * 1024)   // 32KB ring buffer in PSRAM
#define USB_BUFFER_SIZE           PSRAM_RING_BUFFER_SIZE   // Full PSRAM ring buffer
#define POST_JOB_MIN_GUARD_MS   5000
#define POST_JOB_STATUS_WAIT_MAX_MS 10000

// --- Watchdog ---
#define WDT_TIMEOUT_SEC   60      // Watchdog timeout seconds

// --- NVS Storage Keys ---
#define NVS_NAMESPACE     "hp-print"
#define NVS_KEY_SSID      "ssid"
#define NVS_KEY_PASS      "pass"
#define NVS_KEY_HOSTNAME  "hostname"

#endif // CONFIG_H