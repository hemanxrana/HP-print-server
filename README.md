# HP Print Server

Arduino IDE firmware for an ESP32-S3 print server designed primarily for **smartphone printing** to USB-connected HP printers.

## Smartphone-first design

The network-facing side is built around the protocols used by Android's built-in Default Print Service / Mopria-style driverless printing:

```text
Android phone
    |
    | DNS-SD discovery
    | IPP over TCP/631
    v
ESP32-S3
    |
    | persistent print queue
    v
mobile document (PWG Raster / PCLm / PDF / JPEG / URF)
    |
    | renderer / printer backend
    v
USB Host -> HP printer
```

Android's built-in printing stack uses mDNS/DNS-SD and IPP for network printer discovery, and the Android/Mopria stack supports mobile print data formats including PWG Raster, PCLm and PDF. The firmware therefore prioritizes a standards-based IPP printer interface instead of requiring an Android-specific app.

## Target printer: HP Smart Tank 520

The HP Smart Tank 520 is an important special case: HP's current specifications list **no built-in wireless networking** and one Hi-Speed USB 2.0 device connection. HP specifies **HP PCL 3 GUI** and **HP PCLm** as its print languages. Therefore the ESP32-S3 is the wireless/network print server and must ultimately provide the USB-host side plus the required document rendering/translation.

That means accepting a PDF over IPP is **not sufficient**. The final backend must turn the mobile document into something the Smart Tank 520 can actually print.

## Current firmware

Implemented on the `chore/initial-project-skeleton` branch:

- Arduino IDE / ESP32-S3 sketch
- Wi-Fi station mode
- fallback configuration access point
- persistent Wi-Fi/printer configuration using Preferences
- mobile-friendly configuration web page
- Wi-Fi scan page
- DNS-SD `_ipp._tcp` advertisement
- Android-compatible IPP resource-path TXT record (`rp=ipp/print`)
- mobile-oriented IPP capability response
- IPP `Print-Job` ingress
- support declarations for PWG Raster, PCLm, PDF, JPEG and URF
- persistent LittleFS print-job spool
- 4 MiB incoming-job safety limit
- one-job-at-a-time queue protection

The incoming document is currently **queued, not yet rendered and sent to the HP printer**. The firmware does not claim successful physical printing until the USB Host and HP PCLm/printer backend are implemented and tested against hardware.

## Why PCLm matters

PCLm is particularly relevant to mobile printing because Android/Mopria supports it and HP uses it in mobile-oriented printing stacks. The Smart Tank 520 also explicitly lists HP PCLm among its supported print languages.

## Configuration

On first boot, connect to:

```text
SSID:     HP-Print-Server
Password: configureme
```

and open:

```text
http://192.168.4.1/
```

Configure the normal Wi-Fi network and printer information. The configuration is stored in ESP32 NVS and survives reboot.

The AP password is a development default and must be made configurable before production use.

## Build with Arduino IDE

1. Install Arduino IDE.
2. Install **esp32 by Espressif Systems** through Boards Manager.
3. Select the board definition matching the physical ESP32-S3 N16R8-class board.
4. Open `HP-print-server.ino`.
5. Compile and upload.
6. Open Serial Monitor at 115200 baud.

The project intentionally uses Arduino IDE rather than PlatformIO.

## Project files

- `HP-print-server.ino` — application, configuration UI, Wi-Fi and service startup
- `mobile_ipp_server.h/.cpp` — mobile IPP HTTP/IPP server
- `mobile_print_queue.h/.cpp` — persistent print-job spool
- `mobile_print_profile.h` — Android/Mopria-oriented IPP profile
- `printer_protocols.h` — protocol/discovery definitions retained for the next transport/discovery layers

## Verification status

Source-level verification has been performed against the IPP data-type definitions and Android/Mopria discovery requirements. Hardware compilation/upload and end-to-end printing still require an ESP32-S3 board connected to the development environment.

The next hardware milestone is:

1. ESP32-S3 USB Host initialization.
2. Enumerate the HP Smart Tank 520 as a USB printer.
3. Identify its USB interfaces/endpoints.
4. Implement the HP printer transport.
5. Implement or integrate a correct PCLm/PWG rendering path.
6. Consume the persistent IPP job and report real job state back to the phone.
7. Test PDF, photo, copies, page range, orientation and A4 printing from Android.
