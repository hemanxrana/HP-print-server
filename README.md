# HP Print Server

Arduino IDE firmware for an **ESP32-S3** wireless print server, with an emphasis on USB-connected HP printers and smartphone printing.

## Arduino IDE layout

The project is intentionally **flat**: all `.ino`, `.cpp`, and `.h` files required by the firmware live in the sketch root. There is no nested `src/usb` source tree and no duplicate USB headers.

Open:

```text
HP-print-server.ino
```

in Arduino IDE. Arduino will compile the companion `.cpp` files in the same directory.

## Target stack

```text
Android / Mopria / Default Print Service
          |
          | DNS-SD _ipp._tcp
          | IPP over TCP/631
          v
ESP32-S3 IPP server + persistent LittleFS queue
          |
          | USB Host
          v
USB Printer Class interface selection
          |
          | Bulk OUT
          v
HP printer
```

The USB implementation enumerates the printer descriptors, discovers Printer Class interfaces and Bulk endpoints, scores the available interfaces, and allows an advanced manual interface/alternate-setting selection from the web UI.

For normal HP printers the automatic selector prefers the standard bidirectional Printer Class protocol (`0x02`). Other Printer Class protocols are detected and exposed, but the current raw-PCL backend only sends jobs through a usable raw-print interface.

## Main features

- ESP32-S3 Arduino IDE firmware
- Wi-Fi station mode with fallback configuration AP
- persistent Wi-Fi/printer configuration using Preferences
- browser-based configuration and Wi-Fi scanner
- DNS-SD `_ipp._tcp` advertisement
- IPP smartphone-facing server
- persistent multi-job LittleFS spool
- PWG Raster, PCLm, PDF, JPEG and URF ingress formats
- USB Host enumeration and descriptor inspection
- automatic USB Printer Class interface selection
- manual USB interface + alternate-setting selection
- dynamic Bulk OUT endpoint discovery
- USB device disconnect/reconnect handling
- production USB Bulk OUT transfer path
- PCL test-print endpoint using the production USB backend
- retained RAW 9100, LPR, outbound IPP, mDNS, SSDP, WSD and SNMP transport/discovery helpers
- `/health` status endpoint
- `/simulate` network-to-USB pipeline diagnostic

## USB source files

All USB files are at the sketch root:

- `usb_device.h`
- `usb_host_manager.h/.cpp`
- `usb_printer_backend.h/.cpp`
- `usb_diagnostics.h/.cpp`

This flat layout is deliberate. Do not recreate `src/usb` or keep a second copy of these headers; Arduino IDE can otherwise compile/include inconsistent versions.

## Arduino ESP32 core

The project CI targets **Arduino-ESP32 3.3.10**.

For the ESP32-S3 USB-host portion, use 3.3.10 first. Arduino-ESP32 3.3.11 has a known USB-host enumeration regression in configurations where the enumeration-filter feature is enabled; this firmware includes an allow-all enumeration-filter callback when that feature is present, but 3.3.10 remains the baseline for this project.

## Build

1. Install Arduino IDE.
2. Install **esp32 by Espressif Systems** from Boards Manager.
3. Use Arduino-ESP32 **3.3.10** for the baseline build.
4. Select the ESP32-S3 board matching your hardware.
5. Open `HP-print-server.ino`.
6. Compile and upload.
7. Open Serial Monitor at **115200 baud**.

The repository also contains an Arduino CLI CI workflow that installs ESP32 core 3.3.10 and compiles the sketch as an ESP32-S3 target.

## First boot

The configuration AP is:

```text
SSID:     HP-Print-Server
Password: configureme
```

Open:

```text
http://192.168.4.1/
```

Configure the Wi-Fi network and printer information. The USB interface page appears once a Printer Class device is enumerated.

## USB selection

**Automatic** is the normal setting. The firmware discovers interface numbers, alternate settings and endpoint addresses from the USB descriptors; endpoint addresses are never hard-coded in the configuration.

Manual selection is intended for diagnostics and printers that expose multiple Printer Class interfaces.

A selected interface must have a valid Bulk OUT endpoint before it can be used for raw printing.

## Important status semantics

A network IPP job is not marked completed merely because it was accepted by the TCP/IP layer. The queue remains pending until the USB backend successfully transfers the complete document. A failed USB transfer moves the job to an aborted/error state.

USB transfer acknowledgement means that the USB host accepted the transfer; it is not proof that the printer physically produced a page.

## Current limitations

- The raw USB backend currently targets Printer Class protocol `0x02` (standard bidirectional raw/PCL-style printing).
- Printer Class protocol `0x04` is detected but is not yet implemented as a full IPP-over-USB transport.
- Document conversion/rendering is not a general-purpose PDF/PWG-to-HP-PCL engine; the phone's supplied format must be supported by the selected backend path.
- Physical page verification requires printer-side status/feedback and is not inferred from USB transfer completion alone.

## Repository history

The current Arduino-ready structure consolidates the latest USB interface-selection work rather than preserving the older nested `src/usb` layout. Earlier development branches are retained as historical references, while the Arduino-ready branch is the source of truth for the flat sketch structure.
