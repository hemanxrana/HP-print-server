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

// --- Physical capability claims (HP Smart Tank 520/540) ------------------
// The attached printer is a Smart Tank 520/540 class device: colour inkjet,
// borderless up to A4/Letter, and *manual* two-sided only (no duplexer).
// This bridge forwards the client's PCL, so the client's driver renders the
// colour/monochrome choice and, for manual duplex, drives the paper flip;
// these switches only stop the IPP service from hiding those options.
#define PRINTER_BORDERLESS      1   // advertise 0 margins / borderless media
#define PRINTER_DUPLEX          1   // advertise two-sided (manual on this HW)
// Non-borderless printable margin in hundredths of a millimetre (3.0 mm,
// from the Smart Tank 520 data sheet).
#define PRINTER_PAPER_MARGIN    300

// PCLm passthrough is a temporary experiment: the HP plugin can rasterise
// PCLm on the host, and HP's own firmware accepts it, but we do not know yet
// whether the Smart Tank accepts PCLm over its USB transport.  We only forward
// the IPP body verbatim, exactly like PCL3GUI.  Set to 0 to go back to PCL only.
#define PRINTER_PCLM_PASSTHROUGH 1
#define PRINTER_PCLM_MIME        "application/PCLm"  // casing from a real HP dump
// MIME types are case-insensitive; the printer-device-id CMD token is the
// HP-specific capability hint a print app is most likely to key on.
#if PRINTER_PCLM_PASSTHROUGH
#define PRINTER_DEVICE_ID      "MFG:HP;MDL:Smart Tank 520;CMD:PCL3GUI,PCLM;CLS:PRINTER;STATUS;"
#define PRINTER_DOCUMENT_VERSION "PCL3GUI,PCL3,PJL,Automatic,PCLM"
#define PRINTER_FORMAT_LIST_IPP  "application/vnd.hp-PCL,application/PCLm,application/octet-stream"
#else
#define PRINTER_DEVICE_ID      "MFG:HP;MDL:Smart Tank 520;CMD:PCL3GUI;CLS:PRINTER;STATUS;"
#define PRINTER_DOCUMENT_VERSION "PCL3GUI,PCL3,PJL,Automatic"
#define PRINTER_FORMAT_LIST_IPP  "application/vnd.hp-PCL,application/octet-stream"
#endif
#define PRINTER_DOCUMENT_FORMAT  "application/vnd.hp-PCL"
// Capability lists advertised via Bonjour TXT records and IPP attributes.
// RAW queues display PCL3GUI explicitly so HP print apps show the dialect.
#if PRINTER_PCLM_PASSTHROUGH
#define PRINTER_FORMAT_LIST_RAW  "application/vnd.hp-PCL,application/PCLm,application/octet-stream,PCL3GUI"
#else
#define PRINTER_FORMAT_LIST_RAW  "application/vnd.hp-PCL,application/octet-stream,PCL3GUI"
#endif

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
// A full Get-Printer-Attributes response (all media collections + the HP
// plugin's ~68 requested names) exceeds 8 KB.  16 KB leaves headroom without
// eating into the 300 KB internal-heap budget.
#define IPP_RESPONSE_MAX          16384             // single IPP response buffer (one client at a time)
#define IPP_JOB_DEADLINE_MS       600000UL          // hard cap on one document body

// --- Watchdog ---
#define WDT_TIMEOUT_SEC   60      // Watchdog timeout seconds

// --- NVS Storage ---
#define NVS_NAMESPACE       "hp-print"
#define NVS_KEY_SSID        "ssid"
#define NVS_KEY_PASS        "pass"

#endif // CONFIG_H
