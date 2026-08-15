# HP Print Server

Arduino IDE firmware for an ESP32-S3 print server designed primarily for **smartphone printing** to USB-connected HP printers.

## Smartphone-first architecture

```text
Android / Mopria / Default Print Service
        |
        | DNS-SD _ipp._tcp
        | IPP over TCP/631
        v
ESP32-S3 IPP endpoint /ipp/print
        |
        | persistent LittleFS job store
        v
 pending -> processing -> completed / canceled / aborted
        |
        v
 USB Host -> HP printer backend
```

The network side is intentionally independent from the printer-side transport. This lets the ESP32 present a standards-based IPP printer to a phone while the eventual backend handles the USB-connected HP device.

## Target printer: HP Smart Tank 520

HP's current specifications list **no built-in wireless networking**, one Hi-Speed USB 2.0 device connection, and **HP PCL 3 GUI / HP PCLm** print languages. Therefore the ESP32-S3 must provide the wireless network service, USB Host transport, and a document path that produces a format the printer accepts.

Accepting a PDF or PWG Raster job over IPP is therefore only the network-ingress half of the project. The firmware deliberately does **not** claim that a physical page printed until a printer backend reports that state.

## Implemented on `chore/initial-project-skeleton`

- Arduino IDE / ESP32-S3 sketch
- Wi-Fi station mode with fallback configuration AP
- persistent Wi-Fi and printer configuration using Preferences
- mobile configuration web page and Wi-Fi scanner
- DNS-SD `_ipp._tcp` advertisement with `rp=ipp/print`
- IPP 1.1/2.0 request handling over HTTP
- `Print-Job`
- `Validate-Job`
- `Cancel-Job`
- `Get-Job-Attributes`
- `Get-Jobs`
- `Get-Printer-Attributes`
- requested-attributes handling for the implemented attribute set
- `which-jobs` and `limit` handling for Get-Jobs
- persistent multi-job LittleFS spool
- up to 8 active jobs, with terminal jobs retained for IPP history
- reboot recovery: a job left in `processing` is returned to `pending`
- cancellation and persistent job-state reasons
- 4 MiB per-document safety limit
- accepted mobile formats: PWG Raster, PCLm, PDF, JPEG and URF
- strict HTTP request-path/content-type/content-length checks
- no false `completed` state before a real backend reports completion
- retained printer-discovery layer for mDNS, SSDP, WSD and targeted SNMP
- retained RAW 9100 and LPR transport interfaces
- corrected IPP transport request encoder for outbound printer-to-printer use

The IPP behavior follows the IPP operation and job-state model in RFC 8011: Validate-Job does not create a job, Get-Jobs supports completed/not-completed selection, Cancel-Job transitions a live job to canceled, and Get-Job-Attributes exposes the retained job object.

## Current boundary

The current firmware is a **working network-facing print-server layer**, not yet a complete physical HP printer driver.

Still required for real paper output:

1. ESP32-S3 USB Host initialization and enumeration of the Smart Tank 520.
2. USB endpoint/interface identification and bulk transport.
3. HP PCLm/PCL 3 GUI-compatible rendering/translation where the phone does not already supply a printer-compatible document.
4. A backend worker that changes jobs from `pending` to `processing` and finally `completed` or `aborted` based on real USB/printer status.
5. Hardware testing with the actual Smart Tank 520.

HP documents the Smart Tank 520 as supporting PCLm, which is why PCLm is the preferred direct mobile-to-HP path.

## Configuration

First boot creates:

```text
SSID:     HP-Print-Server
Password: configureme
```

Open:

```text
http://192.168.4.1/
```

Configure the normal Wi-Fi network and printer information. Configuration survives reboot.

The AP password remains a development default and should be made configurable before production use.

## Arduino IDE build

1. Install Arduino IDE.
2. Install **esp32 by Espressif Systems** through Boards Manager.
3. Select the ESP32-S3 board definition matching the physical board.
4. Open `HP-print-server.ino`.
5. Compile and upload.
6. Open Serial Monitor at 115200 baud.

## Project files

- `HP-print-server.ino` — application, configuration UI, Wi-Fi and service startup
- `mobile_ipp_server.h/.cpp` — HTTP + IPP smartphone-facing server
- `mobile_print_queue.h/.cpp` — persistent multi-job spool and recovery
- `mobile_print_profile.h` — mobile IPP/DNS-SD profile
- `discovery_engine.h/.cpp` — mDNS, SSDP, WSD and targeted SNMP discovery
- `print_transports.h/.cpp` — RAW 9100, LPR and outbound IPP transport helpers
- `ipp_server.h/.cpp` — legacy IPP compatibility layer; mobile printing uses `mobile_ipp_server`
- `printer_protocols.h` — transport/discovery enums

## Verification

The current source was re-reviewed after implementation specifically for the failure modes found during the earlier simulation: single-job storage, incorrect IPP operation handling, missing Get-Jobs, incorrect job-state semantics, cancellation, reboot recovery, queue capacity, requested attributes, Get-Jobs filtering, and false completion. The previously removed discovery/transport files were also restored so the protocol roadmap is not silently lost.

A physical Arduino build and end-to-end Android print test still require the ESP32-S3 development board and Arduino ESP32 toolchain. The repository should therefore be treated as **network-layer implementation complete, hardware backend not yet complete**, rather than as a finished printer driver.
