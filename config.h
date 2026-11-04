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
#define PRINTER_MAKE_MODEL     "HP Smart Tank 520"
// Model string matters: print apps like NetPrinter match it against a device
// database (entry "Smart Tank 2xx/5xx"), so keep suffixes off the model name.
#define PRINTER_DEVICE_ID      "MFG:HP;MDL:Smart Tank 520;CMD:PCL3GUI;CLS:PRINTER;STATUS;"
#define PRINTER_DOCUMENT_FORMAT  "application/vnd.hp-PCL"
#define PRINTER_DOCUMENT_VERSION "PCL3GUI"
// Capability lists advertised via Bonjour TXT records and IPP attributes.
// RAW queues display PCL3GUI explicitly so HP print apps show the dialect.
#define PRINTER_FORMAT_LIST_RAW  "application/vnd.hp-PCL,application/octet-stream,PCL3GUI"
#define PRINTER_FORMAT_LIST_IPP  "application/vnd.hp-PCL,application/octet-stream"

// --- USB Bulk Out Settings ---
// ESP32-S3 has USB 2.0 Full Speed only (12 Mbps), so ~1 MB/s is the bus ceiling.
// 16KB transfers amortise per-transfer overhead without wasting internal DMA RAM.
#define BULK_OUT_CHUNK_SIZE       (16 * 1024)   // transfer size, no MPS alignment
#define PSRAM_RING_BUFFER_SIZE    (64 * 1024)   // RAW 9100 staging ring in PSRAM
#define USB_BUFFER_SIZE           PSRAM_RING_BUFFER_SIZE   // Full PSRAM ring buffer
#define USB_BULK_PIPELINE_DEPTH   3             // outstanding bulk-out transfers
// Post-job settle: the printer reports "selected" while its own buffer drains,
// so a short guard is enough. The long outer bound still covers slow prints.
#define POST_JOB_MIN_GUARD_MS   1500
#define POST_JOB_STATUS_WAIT_MAX_MS 10000

// --- USB Streaming Writer ---
// A dedicated task feeds usb_host_transfer_submit() so printer backpressure
// cannot stall the Arduino loop (HTTP/mDNS/status).
#define USB_STREAM_BUFFER_SIZE    (64 * 1024)   // PSRAM staging ring
#define USB_STREAM_TASK_STACK     6144          // bytes (ESP-IDF semantics)
#define USB_STREAM_TASK_PRIORITY  5
#define USB_STREAM_TASK_CORE      0
#define USB_STREAM_PUSH_TIMEOUT_MS 30000        // matches RAW_USB_TIMEOUT_MS

// --- IPP Streaming Settings ---
#define IPP_BODY_BUFFER_SIZE      USB_BUFFER_SIZE   // document streaming buffer
#define IPP_CLIENT_TIMEOUT_MS     30000             // < WDT (60 s) so a stalled client disconnects
#define IPP_RESPONSE_MAX          8192              // single IPP response buffer (one client at a time)
#define IPP_JOB_DEADLINE_MS       600000UL          // hard cap on one document body

// --- Watchdog ---
#define WDT_TIMEOUT_SEC   60      // Watchdog timeout seconds

// --- NVS Storage ---
#define NVS_NAMESPACE       "hp-print"
#define NVS_KEY_SSID        "ssid"
#define NVS_KEY_PASS        "pass"

#endif // CONFIG_H
