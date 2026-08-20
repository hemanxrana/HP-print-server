# HP Print Server — ESP32-S3 RAW 9100

Arduino IDE firmware for an **ESP32-S3** wireless print server for USB-connected printers, currently focused on transparent HP RAW/JetDirect-style printing.

## Architecture

```text
PC / Android / network print client
          |
          | TCP 9100 (JetDirect / AppSocket)
          v
ESP32-S3 Wi-Fi RAW server
          |
          | byte-for-byte USB Bulk OUT
          v
classic USB Printer Class interface
          |
          v
HP printer
```

The print path is intentionally small:

- TCP 9100 only
- no IPP server
- no document conversion
- no print queue or spool
- no automatic PJL/UEL/form-feed injection
- no scanner support
- USB Printer Class interfaces and Bulk endpoints are discovered from descriptors
- the firmware automatically selects the best RAW-compatible classic Printer Class interface

## USB protocol policy

For RAW printing, the firmware accepts classic USB Printer Class protocols:

- `0x01` — unidirectional
- `0x02` — bidirectional, preferred
- `0x03` — IEEE 1284.4-compatible

Printer Class protocol `0x04` is **IPP-over-USB** and is deliberately not used as a transparent RAW transport. Vendor-specific protocol `0xFF` is also ignored unless a future device-specific implementation explicitly verifies it.

The selected interface is automatic; there is no user-facing USB interface selector.

## Important RAW limitation

TCP 9100 is a transport, not a document format. The data sent to port 9100 must already be a print stream understood by the connected printer, such as a supported PCL/PJL stream.

The firmware deliberately forwards bytes unchanged. A successful USB Bulk OUT transfer proves that the ESP32 delivered the bytes to the printer; it does not prove that the printer understood the document or physically completed the page.

## Configuration

On first boot, or when saved Wi-Fi cannot be reached, the ESP32 starts a setup access point:

```text
SSID:     HP-Print-Server
Password: configureme
```

Open:

```text
http://192.168.4.1/
```

After Wi-Fi configuration, the dashboard is available at the assigned IP address and, when mDNS starts successfully:

```text
http://printer.local/
```

The dashboard provides:

- printer connection and readiness
- human-readable USB printer status
- Wi-Fi network and IP address
- RAW 9100 status
- printer address for AppSocket/JetDirect setup
- Wi-Fi scanning and configuration

## Printing

Connect a print client to:

```text
<ESP32-IP>:9100
```

or, when mDNS works on the client:

```text
printer.local:9100
```

A Windows Standard TCP/IP printer port can use RAW protocol on port `9100`.

## USB behavior

The USB host implementation:

- enumerates classic USB Printer Class interfaces
- discovers Bulk OUT/Bulk IN endpoints dynamically
- prefers protocol `0x02`
- uses alternate setting `0` only as a tie-breaker, not as a hard-coded requirement
- optionally uses a separate suitable Printer Class interface for `GET_PORT_STATUS`
- handles USB disconnect/reconnect
- suppresses repeated identical status logging

## RAW job handling

The server accepts one active TCP 9100 client at a time.

- TCP receive data is forwarded in bounded chunks so the main loop remains responsive.
- USB writes are synchronously checked for completion.
- After TCP closes, a short non-blocking drain period prevents a following job from colliding with the printer's final USB consumption.
- Aborted jobs reset their byte accounting before the next job.

## Wi-Fi behavior

- Saved Wi-Fi is attempted at boot.
- If connection fails, the configuration AP is started.
- The firmware periodically retries the saved Wi-Fi network.
- If Wi-Fi later recovers, the temporary configuration AP is stopped.
- The dashboard only shows `printer.local` when mDNS actually started successfully.

## Arduino ESP32 core

Project baseline:

- Arduino IDE
- `esp32` by Espressif Systems
- Arduino-ESP32 **3.3.10**
- ESP32-S3 board target
- Serial Monitor: **115200 baud**

Open `HP-print-server.ino`, compile, upload, and test with the real printer.

## Repository policy

Only normal source code and the Arduino compile workflow should live in the repository. Temporary source-editing workflows and scratch files are intentionally excluded.
