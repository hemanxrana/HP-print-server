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
        | pipelined USB Bulk OUT (2x1KB transfers in flight, 1KB chunks)
        v
classic USB Printer Class interface
        |
        v
HP printer (PCL3GUI via CMD: in the 1284.4 device-id)
```

- No document conversion, no spool, no PJL/UEL/form-feed injection by default.
- IPP accepts `application/vnd.hp-pcl` (case-insensitive) and forwards it to USB
  unchanged. `application/octet-stream` is also accepted as an explicit raw
  passthrough for clients that do not name a PCL MIME type. Any other
  `document-format` is rejected with IPP status `0x040A` so the client is told
  clearly and quickly.
- PCLm passthrough was a temporary experiment and has been withdrawn: the
  Smart Tank 520/540 does not accept PCLm over its USB transport (the
  printer printed a partial page and faulted). `PRINTER_PCLM_PASSTHROUGH` is
  now 0 in `config.h`; the service logic is unchanged and the claim is simply
  no longer advertised.
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
| `_ipp._tcp` | 631 | `rp=ipp/print`, `txtvers=1`, `qtotal=1`, `device-id` (MFG/MDL/`CMD:PCL3GUI` 1284.4 string), `pdl=application/vnd.hp-PCL`, `product`, `ty`, `Color=T`, `Duplex=T[,F]`, `note`, `UUID`, `priority=0`, `air=none`, `adminurl=http://<ip>/` |
| `_pdl-datastream._tcp` | 9100 | `pdl=application/vnd.hp-PCL` |

The AirPrint `_universal` subtype is deliberately **not** advertised: a
`URF`/universal claim would advertise a raster format this bridge cannot
print. The Android HP Print Service Plugin browses plain `_ipp._tcp` and
vendor-checks the `device-id` TXT, which is fully covered.

NetPrinter (and some other Android apps) display only the single PDL they
auto-select for a discovered printer. With PCLm withdrawn, only PCL 3 GUI is
advertised, so the discovery card shows the native dialect directly.

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
- Wi-Fi scan / connect (DHCP only; credentials persisted in NVS)
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
- every interface and endpoint is logged at enumeration (class/subclass/
  protocol, endpoint type, direction, packet size); interrupt-IN status
  notification endpoints are recorded for diagnostics
- automatic selection of the best RAW-compatible interface and, when separate,
  a suitable status interface for `GET_PORT_STATUS`
- pipelined bulk-out: up to two 1 KB transfers are always in flight; a job
  tail is drained exactly once before the post-job guard starts
- timeout recovery (halt/flush/clear) and, for unrecoverable wedges,
  automatic USB re-enumeration
- optional compile-time `RAW_APPEND_JOB_TERMINATOR` appends a PJL
  end-of-job plus form feed (disabled by default)

## RAW job handling

- one TCP 9100 client at a time; bounded chunk size keeps the loop responsive
- stalled clients are disconnected after 60 s without progress (a connection
  that sends no print data at all is closed after 30 s)
- post-job guard waits for the printer's port status to settle

## IPP job handling

- the document body is consumed by a cooperative state machine driven from
  `loop()` (`pumpDocument`), not inside a blocking request handler. USB status
  polling, mDNS, the dashboard, Wi-Fi maintenance and TCP 9100 accept keep
  running while an IPP job is transferring or printing.
- safe hybrid: below 512 KB the body is staged in PSRAM **in full** before the
  first byte is handed to USB, so a client that aborts, sends a short body or
  breaks chunk framing never reaches the printer (the job fails with `0x0500`
  and nothing is printed). Above 512 KB the document streams while the
  remaining body is still arriving, which keeps large jobs latency-free.
- the staging buffer grows on demand to 4 MB; once full, receive pauses until
  the USB side drains it (bounded backpressure instead of unbounded memory).
- if the source ends early *after* bytes have already reached USB (only
  possible for streamed documents), the stream is terminated cleanly with the
  normal ZLP/SOFT_RESET instead of dropping the queued tail mid-raster. A real
  USB failure still goes through the abort/recovery path.
- one IPP document at a time; a second IPP connection waits in the listen backlog
  until the active job has been answered. `Cancel-Job` for the active job is
  therefore handled after the document finishes.
- while an IPP document is uploading or printing, RAW 9100 connections are
  refused and IPP `Create-Job`/`Send-Document` are refused while a RAW client
  owns the pipe, so the two sources can never interleave on USB.
- the post-job guard keeps the following job at `0x0503` until the printer has
  finished the previous one.
- `GET_PORT_STATUS` continues to use the separate Printer Class status
  interface when the device exposes one, so status requests never share the
  Bulk OUT pipe used for print data.

## Job history

- the last 8 RAW/IPP jobs are stored in NVS (`joblog` namespace) and survive
  reboot. Each entry records source, job id, state, bytes handed to USB,
  duration, terminal reason, transport detail, client IP and document format.
- a job is written as `job-incoming` when it starts and folded into the final
  state when it finishes, so a crash/reboot in the middle of a print still
  leaves evidence.
- inspect it at `GET /jobs.json` (newest first); `/status.json` also carries
  `jobLogCount` and a one-line `lastJobLog`. The newest entry is logged on the
  serial console at boot.

## Wi-Fi behavior

- Wi-Fi modem power save is disabled so print traffic is not delayed.
- Retries the saved network every 30 s; configuration AP stops once the
  station links up; on every reconnect the RAW and IPP listeners are re-bound
  and mDNS is re-announced.
- mDNS responder health: if the responder ever fails to start, discovery is
  retried periodically until it succeeds.
- 60 s task watchdog reboots the device if the main loop wedges.

## Building

- Arduino IDE / arduino-cli, **arduino-esp32 core 3.3.12**
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
