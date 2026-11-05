# HP Print Server — ESP32-S3 USB print bridge (RAW 9100 + IPP 631)

Arduino IDE firmware for an **ESP32-S3** wireless print server that bridges
network print traffic to a USB-connected HP printer. Data is streamed
byte-for-byte into the classic USB Printer Class Bulk OUT endpoint; the printer
is emulated on the network side as an HP Smart Tank that speaks PCL3GUI.

## Architecture

```text
print client (Android HP Print Service, Windows, Linux, CUPS)
        |
        |  RAW JetDirect/AppSocket  TCP 9100        (byte-for-byte passthrough)
        |  IPP 1.1 (2.x accepted)   TCP 631         (PCL3GUI-only wrapper)
        |  mDNS / Bonjour           printer.local   (_ipp._tcp + _pdl-datastream._tcp)
        v
ESP32-S3 Wi-Fi servers
        |
        | pipelined USB Bulk OUT (2x8KB transfers in flight, 8KB chunks)
        v
classic USB Printer Class interface
        |
        v
HP printer (PCL3GUI via CMD: in the 1284.4 device-id)
```

- No document conversion, no spool, no PJL/UEL/form-feed injection by default.
- IPP accepts `application/vnd.hp-pcl` (case-insensitive) and, while
  `PRINTER_PCLM_PASSTHROUGH` is enabled, `application/PCLm`; both are forwarded
  to USB unchanged. Any other `document-format` is rejected with IPP status
  `0x040A` so the client is told clearly and quickly.
- PCLm support is a **temporary experiment**: the host rasterises PCLm and HP
  firmware consumes it, but whether this printer accepts PCLm over USB is
  unproven. Set `PRINTER_PCLM_PASSTHROUGH 0` in `config.h` to withdraw the
  claim without touching the service logic.
- Emulated printer identity lives in `config.h` as the single source of truth
  (`PRINTER_INSTANCE_NAME`, `PRINTER_MAKE_MODEL`, `PRINTER_DEVICE_ID`,
  `PRINTER_DOCUMENT_FORMAT`) and is mirrored into both mDNS TXT records and
  the IPP `Get-Printer-Attributes` response.
- The physical printer is a Smart Tank 520/540 class device, so the IPP
  capability set mirrors it: colour and monochrome, **borderless** up to
  A4/Letter (`media-*-margin-supported` includes 0), and **manual**
  two-sided printing (`sides-supported` plus `manual-duplex-supported`).
  There is no duplexer, so two-sided means the client's driver prints the
  second side after the user flips the sheet; the bridge only forwards PCL.
  Set `PRINTER_BORDERLESS` or `PRINTER_DUPLEX` to 0 in `config.h` to hide
  either claim.

## Discovery (mDNS / Bonjour)

When Wi-Fi is up the device advertises `printer.local` with two services:

| service | port | TXT records |
|---|---|---|
| `_ipp._tcp` | 631 | `rp=ipp/print`, `txtvers=1`, `qtotal=1`, `device-id` (MFG/MDL/`CMD:PCL3GUI[,PCLM]` 1284.4 string), `pdl=application/vnd.hp-PCL[,application/PCLm]`, `product`, `ty`, `Color=T`, `Duplex=T[,F]`, `note`, `UUID`, `priority=0`, `air=none`, `adminurl=http://<ip>/` |
| `_pdl-datastream._tcp` | 9100 | `pdl=application/vnd.hp-PCL[,application/PCLm]` |

The AirPrint `_universal` subtype is deliberately **not** advertised: a
`URF`/universal claim would advertise a raster format this bridge cannot
print. The Android HP Print Service Plugin browses plain `_ipp._tcp` and
vendor-checks the `device-id` TXT, which is fully covered.

## Configuration

`config.h` holds all compile-time constants (ports, mDNS hostname, emulated
printer identity, USB chunk/pipeline sizes, watchdog, IPP timeouts). Runtime
settings are managed from the web dashboard and stored in NVS.

On first boot, or when saved Wi-Fi cannot be reached, the device starts a
setup access point:

```text
SSID:     HP-Print-Server
Password: configureme
http://192.168.4.1/
```

The dashboard provides:

- printer connection and readiness, human-readable USB status
- Wi-Fi scan / connect, and DHCP vs static IP configuration
  (/IP, gateway, subnet persisted and applied before association)
- RAW 9100 and IPP 631 status, job counters, last error
- `http://printer.local/` when mDNS started successfully

## Printing

- **RAW/JetDirect:** connect the client to `<ESP32-IP>:9100`
  (Windows Standard TCP/IP port, RAW protocol).
- **IPP / Android HP Print Service:** the printer appears automatically as
  discoverable via mDNS; jobs must be emitted as
  `application/vnd.hp-pcl` (PCL3GUI). If a RAW 9100 client holds the stream,
  an arriving IPP job is rejected with `0x0503 server-error-busy` so the
  client retries instead of dropping the printer.

## USB behavior

- USB Printer Class interfaces are discovered from descriptors
  (protocols 0x01/0x02/0x03 are accepted, 0x02 preferred; 0x04 IPP-over-USB
  and vendor-specific 0xFF are ignored)
- automatic selection of the best RAW-compatible interface and, when separate,
  a suitable status interface for `GET_PORT_STATUS`
- pipelined bulk-out: up to two 8 KB transfers are always in flight; a job
  tail is drained exactly once before the post-job guard starts
- timeout recovery (halt/flush/clear) and, for unrecoverable wedges,
  automatic USB re-enumeration
- optional compile-time `RAW_APPEND_JOB_TERMINATOR` appends a PJL
  end-of-job plus form feed (disabled by default)

## RAW job handling

- one TCP 9100 client at a time; bounded chunk size keeps the loop responsive
- stalled clients are disconnected after 30 s without progress
- post-job guard waits for the printer's port status to settle

## Wi-Fi behavior

- Wi-Fi modem power save is disabled so print traffic is not delayed.
- Retries the saved network every 30 s; configuration AP stops once the
  station links up; on every reconnect the RAW and IPP listeners are re-bound
  and mDNS is re-announced.
- mDNS responder health: if the responder ever fails to start, discovery is
  retried periodically until it succeeds.
- 60 s task watchdog reboots the device if the main loop wedges.

## Building

- Arduino IDE / arduino-cli, **arduino-esp32 core 3.3.10**
- Board: ESP32-S3 N16R8 (16 MB flash, 8 MB octal PSRAM)
- CI flags: `esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi`
- Serial monitor: **115200 baud**

Changing the partition scheme requires one full flash over USB; OTA (if
used later) can only update inside the existing layout.

## Testing with the HP Print Service Plugin

Watch the serial log for these markers:

1. `[mDNS] ...` — `_ipp._tcp` advertisement and responder heap usage.
2. `[USB] Persistent Bulk OUT pipeline: 2 transfer buffers` — pipelining armed.
3. `[IPP] op=0x000B ...` — the plugin's Get-Printer-Attributes (vendor check
   against `printer-device-id`).
4. `[IPP] op=0x0002 ... format=...` — **the empirical question**: which
   `document-format` the plugin actually emits for a PCL3GUI-only printer.
5. `[IPP][PERF] job=... avg=...KiB/s ... usb-send=...` — throughput split
   between network read and USB send.
6. `[USB] Bulk OUT ... failed ...` — anything here during a job needs the
   status, accepted/total pair from the log line.

## Repository policy

Only normal source code and the Arduino compile workflow live in the
repository; scratch files are intentionally excluded.
