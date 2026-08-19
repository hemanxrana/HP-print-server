# HP Print Server — ESP32-S3 RAW 9100

Arduino IDE firmware for an **ESP32-S3** wireless print server for USB-connected printers, currently focused on transparent HP RAW/JetDirect-style printing.

## Current architecture

```text
PC / Android / network print client
          |
          | TCP 9100 (JetDirect / AppSocket)
          v
ESP32-S3 Wi-Fi RAW server
          |
          | byte-for-byte USB Bulk OUT
          v
USB Printer Class interface
          |
          v
HP printer
```

The network printing path is intentionally small:

- **TCP 9100 only**
- no IPP server
- no HTTP print protocol
- no Content-Length handling for print data
- no print queue or LittleFS spool
- no PDF/PWG/URF/PCLm conversion
- no print-language modification
- USB Printer Class interface and Bulk OUT endpoint are discovered from descriptors
- automatic interface selection prefers the standard bidirectional Printer Class protocol `0x02`
- manual interface + alternate-setting selection is available from the configuration page

The ESP32 forwards the received bytes unchanged to the selected USB Bulk OUT endpoint.

## Important RAW limitation

TCP 9100 is a **transport**, not a document format. The data sent to port 9100 must already be a print stream understood by the printer, such as a valid PCL/PJL stream supported by the connected HP printer.

The firmware deliberately does **not** append a form-feed, PJL `EOJ`, UEL, or other bytes at the end of a job. Adding such bytes would make the server non-transparent and can corrupt formats it does not understand.

After the TCP connection ends, the firmware waits briefly for the final USB transfer to drain before returning the printer backend to Ready. This is transport cleanup only; it does not change the print data.

## Configuration

On first boot, if no Wi-Fi credentials are saved, the ESP32 starts:

```text
SSID:     HP-Print-Server
Password: configureme
```

Open:

```text
http://192.168.4.1/
```

After Wi-Fi configuration, the web page is available at the ESP32's assigned IP address.

The configuration page provides:

- Wi-Fi SSID/password
- nearby Wi-Fi scan
- USB printer/interface information
- automatic USB interface selection
- manual interface/alternate-setting selection
- RAW 9100 status

## Printing

Connect the client to:

```text
<ESP32-IP>:9100
```

The firmware accepts a TCP stream and forwards it directly to USB.

For example, a Windows Standard TCP/IP printer port can target the ESP32 IP address with RAW protocol on port `9100`.

## USB

The USB implementation:

- enumerates USB Printer Class interfaces
- discovers Bulk OUT/Bulk IN endpoints dynamically
- prefers protocol `0x02` when multiple Printer Class interfaces exist
- supports manual interface selection for diagnostics
- handles USB disconnect/reconnect
- uses the production USB Bulk OUT transfer path

For the HP Smart Tank 520/540-family device tested during development, the useful interface was:

```text
Printer Class protocol: 0x02
Bulk OUT: 0x08
Bulk IN:  0x89
```

Endpoint numbers are **not hard-coded**; the firmware obtains them from the USB descriptors.

## Arduino ESP32 core

Use **Arduino-ESP32 3.3.10** as the project baseline.

For the ESP32-S3 USB-host portion:

1. Install Arduino IDE.
2. Install `esp32` by Espressif Systems.
3. Use Arduino-ESP32 3.3.10.
4. Select the appropriate ESP32-S3 board.
5. Open `HP-print-server.ino`.
6. Compile and upload.
7. Open Serial Monitor at 115200 baud.

## What was removed

The RAW-only firmware no longer contains the previous mobile IPP stack, IPP compatibility profiles, IPP queue/spool, outbound IPP transport, or legacy network-printer discovery helpers. This keeps the Arduino sketch focused on the one active print path: **TCP 9100 → USB Bulk OUT**.

## Physical printer errors

A successful USB Bulk OUT transfer only proves that the ESP32 USB host delivered the bytes to the printer. It does not prove that the printer accepted the document format or physically completed the page.

If the printer reports a paper/page/format error after a RAW job, the first thing to verify is the **actual byte stream sent to TCP 9100**. The ESP32 intentionally does not convert or repair that stream.
