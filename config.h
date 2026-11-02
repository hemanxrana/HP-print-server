#ifndef CONFIG_H
#define CONFIG_H

// ============================================================
// ESP32-S3 HP Print Server - Configuration
// Target printer: HP Smart Tank 520
// Board: ESP32-S3-N16R8
//
// Compile-time constants live here as the single source of
// truth.  Runtime settings (Wi-Fi credentials, IP mode) are
// managed from the dashboard and stored in NVS instead.
// ============================================================

// --- Configuration AP (shown only when no saved Wi-Fi connects) ---
#define AP_SSID       "HP-Print-Server"
#define AP_PASSWORD   "configureme"

// --- Print Server Constants ---
#define RAW_SOCKET_PORT    9100    // JetDirect/AppSocket protocol port
#define IPP_PORT           631     // IPP PCL3GUI-only print service port
#define WEB_SERVER_PORT    80      // Web management interface port

// --- Emulated HP identity (mirrored to IPP + mDNS so clients vendor-check us) ---
#define PRINTER_INSTANCE_NAME  "HP Smart Tank"
#define PRINTER_MAKE_MODEL     "HP Smart Tank 520_540 series"
#define PRINTER_DEVICE_ID      "MFG:HP;MDL:Smart Tank 520;CMD:PCL3GUI;CLS:PRINTER;STATUS;"
#define PRINTER_DOCUMENT_FORMAT "application/vnd.hp-pcl"
#define PRINTER_DOCUMENT_VERSION "PCL3GUI"

// --- USB Bulk Out Settings ---
#define BULK_OUT_CHUNK_SIZE       (8 * 1024)    // 8KB chunks, no MPS alignment
#define PSRAM_RING_BUFFER_SIZE    (32 * 1024)   // 32KB ring buffer in PSRAM
#define USB_BUFFER_SIZE           PSRAM_RING_BUFFER_SIZE   // Full PSRAM ring buffer
#define USB_BULK_PIPELINE_DEPTH   2             // outstanding bulk-out transfers
#define POST_JOB_MIN_GUARD_MS   5000
#define POST_JOB_STATUS_WAIT_MAX_MS 10000

// --- IPP Streaming Settings ---
#define IPP_BODY_BUFFER_SIZE      USB_BUFFER_SIZE   // document streaming buffer
#define IPP_CLIENT_TIMEOUT_MS     30000             // < WDT (60 s) so a stalled client disconnects

// --- Watchdog ---
#define WDT_TIMEOUT_SEC   60      // Watchdog timeout seconds

// --- NVS Storage ---
#define NVS_NAMESPACE       "hp-print"
#define NVS_KEY_SSID        "ssid"
#define NVS_KEY_PASS        "pass"

#endif // CONFIG_H
