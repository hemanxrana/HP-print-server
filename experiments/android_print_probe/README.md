# Android print discovery probe

This is a **temporary standalone diagnostic firmware**, not the normal print/scanner server.

It is designed to answer one question with evidence: **what does an Android print service actually try to connect to and send after discovering this ESP32-S3?**

## What it does

- Reuses the Wi-Fi SSID/password already saved by the normal firmware in the `hp-print` Preferences namespace.
- If saved Wi-Fi cannot be used, starts AP `HP-Print-Probe` with password `probe1234`.
- Advertises the real model string `HP Smart Tank 520_540 series`.
- Advertises `_ipp._tcp` on TCP 631.
- Advertises `_pdl-datastream._tcp` on TCP 9100.
- Listens on both ports and captures the beginning of each connection.
- Decodes the HTTP request line and the 8-byte IPP request header when present, including common operation IDs such as `Get-Printer-Attributes`, `Validate-Job`, and `Print-Job`.
- Shows captures on `http://printer.local/` and in Serial Monitor at 115200 baud.

## Important

**USB is intentionally disabled in this firmware.** Nothing captured on 9100 is forwarded to the physical printer. This keeps the experiment isolated from the known-good USB print/scanner code.

The IPP listener intentionally replies with HTTP `501 Not Implemented` after capturing a request. Phase 1 is observation only. Once we know the exact first IPP operation/attributes Android sends, we can implement only the responses needed to let the client progress to its next step.

## Arduino IDE test

1. Open `experiments/android_print_probe/android_print_probe.ino` as its own sketch.
2. Use the same ESP32-S3 board settings as the main project and Arduino-ESP32 3.3.10.
3. Flash it.
4. Open Serial Monitor at 115200.
5. Confirm the board joins the same Wi-Fi as the Android phone.
6. On Android, open **HP Print Service Plugin** or **Default Print Service** and search/add printers.
7. Also open `http://printer.local/` from the phone or PC. The page refreshes every two seconds and shows the latest IPP and RAW captures.
8. Try opening the print dialog and selecting the discovered printer if Android shows it.
9. Copy the Serial output or screenshot the probe page and send it back for analysis.

## What we are looking for

Useful outcomes include:

- Android discovers nothing: mDNS/TXT records need adjustment before IPP matters.
- TCP 631 connection with `Get-Printer-Attributes`: discovery succeeded; next step is a minimal real IPP response.
- Additional IPP operations such as `Validate-Job`, `Create-Job`, or `Print-Job`: we can reconstruct the exact negotiation path.
- TCP 9100 connection: the app is willing to use RAW/JetDirect. The first bytes will reveal whether the payload is PJL/PCL/PCL3GUI/PCLm/another format.

## Optional capture directly on Android

PCAPdroid can capture application traffic without root by using Android's local VPN API. Filter the capture to the HP Print Service (or the system print service), reproduce discovery/printing, then export the PCAP. A PCAP is useful because it also shows DNS/mDNS and connections the ESP32 probe may not be listening for.

For the first run, the ESP32 probe logs are usually easier because they immediately tell us whether ports 631 or 9100 were contacted.


## One-flash diagnostic dashboard

The probe now boots in Safe Capture mode and can switch at runtime between Safe Capture, Classic USB RAW, and experimental IPP-over-USB. The dashboard exposes protocol-0x04 interface candidates, last Android IPP request, PCLm capture completeness, USB bytes accepted, stack/heap telemetry, and the recommended next action. No reflash is required between these transport tests.
