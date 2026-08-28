from pathlib import Path
p=Path('HP-print-server.ino')
s=p.read_text()
s=s.replace('#include "usb_printer_backend.h"\n', '#include "usb_printer_backend.h"\n#include "ipp_pcl3_service.h"\n')
s=s.replace('// ESP32-S3 USB-to-Wi-Fi RAW print server.\n// Network side: JetDirect/AppSocket on TCP 9100 only.\n// Print data is forwarded byte-for-byte to an automatically selected classic\n// USB Printer Class interface. IPP-over-USB/eSCL is intentionally not used.\n', '// ESP32-S3 USB-to-Wi-Fi print server.\n// Network side: RAW JetDirect/AppSocket on TCP 9100 plus IPP on TCP 631.\n// IPP advertises PCL3GUI only and forwards the IPP document payload to the\n// same classic USB Printer Class interface used by RAW printing. No IPP-over-USB\n// interface is selected or used for printing.\n')
s=s.replace('UsbPrinterBackend usbPrinterBackend(usbHost);\n', 'UsbPrinterBackend usbPrinterBackend(usbHost);\nIppPcl3Service ippService(usbPrinterBackend);\n')
needle='''  if (MDNS.addService("pdl-datastream", "tcp", 9100)) {\n    MDNS.addServiceTxt("pdl-datastream", "tcp", "txtvers", "1");\n    MDNS.addServiceTxt("pdl-datastream", "tcp", "note", "RAW 9100");\n    Serial.println("[mDNS] printer.local -> RAW 9100 discovery advertised");\n  }\n'''
repl=needle+'''\n  if (MDNS.addService("ipp", "tcp", 631)) {\n    MDNS.addServiceTxt("ipp", "tcp", "txtvers", "1");\n    MDNS.addServiceTxt("ipp", "tcp", "qtotal", "1");\n    MDNS.addServiceTxt("ipp", "tcp", "rp", "ipp/print");\n    MDNS.addServiceTxt("ipp", "tcp", "ty", "HP Smart Tank 520_540 series");\n    MDNS.addServiceTxt("ipp", "tcp", "product", "(HP Smart Tank 520_540 series)");\n    MDNS.addServiceTxt("ipp", "tcp", "pdl", "application/vnd.hp-PCL");\n    MDNS.addServiceTxt("ipp", "tcp", "usb_MFG", "HP");\n    MDNS.addServiceTxt("ipp", "tcp", "usb_MDL", "HP Smart Tank 520_540 series");\n    MDNS.addServiceTxt("ipp", "tcp", "usb_CMD", "PCL3GUI");\n    Serial.println("[mDNS] printer.local -> IPP 631 advertised as PCL3GUI only");\n  }\n'''
if needle not in s: raise SystemExit('mDNS marker not found')
s=s.replace(needle,repl)
s=s.replace('<a class="btn" href="/scan">Scanner</a>', '<a class="btn" href="/scan">Scanner (disabled)</a>')
s=s.replace('<h2>Print address</h2><p>Use RAW / JetDirect with the printer\'s normal HP driver.</p>', '<h2>Print addresses</h2><p>RAW remains available; IPP advertises only HP PCL3GUI.</p>')
raw_line='  html += mdnsReady ? "socket://printer.local:9100" : (ip.length() ? String("socket://") + ip + ":9100" : "Unavailable");\n'
if raw_line not in s: raise SystemExit('RAW dashboard address marker not found')
s=s.replace(raw_line, raw_line + '  html += R"HTML(</div></div><div class="service" style="margin-top:8px"><div class="label">IPP · PCL3GUI only</div><div class="address">ipp://printer.local:631/ipp/print</div>)HTML";\n', 1)
s=s.replace('''  Serial.println("=== ESP32-S3 RAW 9100 USB Print Server ===");\n  Serial.println("[MODE] RAW JetDirect/AppSocket only; classic USB Printer Class is selected automatically");\n''','''  Serial.println("=== ESP32-S3 HP PCL3GUI Print Server ===");\n  Serial.println("[MODE] RAW 9100 + IPP 631 PCL3GUI; both print through the classic USB Printer Class interface");\n  Serial.println("[MODE] IPP-over-USB printing is disabled/not used; scanner USB backend is disabled");\n''')
s=s.replace('''  usbPrinterBackend.begin();\n\n  configServer.on''','''  usbPrinterBackend.begin();\n  ippService.begin();\n\n  configServer.on''')
s=s.replace('''  Serial.println("[RAW] TCP 9100 server enabled");\n''','''  Serial.println("[RAW] TCP 9100 server enabled");\n  Serial.println("[IPP] TCP 631 service enabled; document-format-supported=application/vnd.hp-PCL; version=PCL3GUI");\n''')
s=s.replace('''  usbPrinterBackend.poll();\n\n  if (millis() - lastStatus''','''  usbPrinterBackend.poll();\n  ippService.poll();\n\n  if (millis() - lastStatus''')
p.write_text(s)
print('main IPP integration applied')
