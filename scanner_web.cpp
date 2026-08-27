#include <Arduino.h>
#include <WebServer.h>
#include "usb_host_manager.h"
#include "usb_scanner_backend.h"

extern WebServer configServer;
extern UsbHostManager usbHost;

namespace {
bool routesInstalled = false;

bool knownSmartTankScanner(const UsbDeviceInfo &d) {
  return d.attached && d.vid == 0x03F0 && d.pid == 0x4554;
}

String scannerName(const UsbDeviceInfo &d) {
  if (!knownSmartTankScanner(d)) return "No scanner detected";
  return "HP Smart Tank 520/540 Scanner";
}

void sendScannerStatusJson() {
  const UsbDeviceInfo &d = usbHost.device();
  const bool detected = knownSmartTankScanner(d);
  String out;
  out.reserve(500);
  out += "{\"detected\":" + String(detected ? "true" : "false");
  out += ",\"escl\":true,\"name\":\"HP Smart Tank 520/540 Scanner\"";
  out += ",\"root\":\"http://printer.local:8080/eSCL/\",\"port\":8080";
  out += ",\"vid\":" + String(d.vid) + ",\"pid\":" + String(d.pid);
  out += ",\"printInterface\":1,\"scannerInterface\":0,\"scannerAlt\":1}";
  configServer.send(200, "application/json", out);
}

void sendScannerPage() {
  const UsbDeviceInfo &d = usbHost.device();
  const bool detected = knownSmartTankScanner(d);

  String html;
  html.reserve(4400);
  html += R"HTML(<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><meta name="theme-color" content="#f3f5f7"><title>Scanner</title><style>
*{box-sizing:border-box}body{margin:0;background:#f3f5f7;color:#344054;font-family:system-ui,-apple-system,"Segoe UI",Roboto,Arial,sans-serif;font-size:14px;line-height:1.5}main{max-width:720px;margin:auto;padding:24px 16px 40px}.top{display:flex;align-items:center;justify-content:space-between;gap:12px;margin-bottom:18px}h1{margin:0;font-size:25px;font-weight:650;letter-spacing:-.02em}.sub,.muted{color:#758195;font-size:13px}.card{background:#fbfcfd;border:1px solid #e1e7ec;border-radius:14px;padding:18px;margin-top:12px}.status{font-size:18px;font-weight:650;margin:5px 0 2px}.dot{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:8px;background:#6f9d7d}.dot.off{background:#b58a62}.grid{display:grid;grid-template-columns:1fr 1fr;gap:9px;margin-top:14px}.item{background:#f6f8fa;border:1px solid #e7ebef;border-radius:10px;padding:11px}.key{font-size:11px;color:#7b8797;text-transform:uppercase;letter-spacing:.045em}.value{margin-top:3px;font-weight:600;word-break:break-word}.btn{display:inline-block;text-decoration:none;color:#466681;background:#edf3f7;border:1px solid #d8e1e8;border-radius:9px;padding:8px 12px;font-weight:600}a{color:#466681}code{font-family:ui-monospace,SFMono-Regular,Consolas,monospace;font-size:12px}@media(max-width:560px){.grid{grid-template-columns:1fr}}</style></head><body><main><div class="top"><div><h1>HP Smart Tank Scanner</h1><div class="sub">eSCL / AirScan over Wi-Fi</div></div><a class="btn" href="/">Dashboard</a></div><div class="card"><div class="key">USB multifunction device</div><div class="status"><span class="dot )HTML";
  html += detected ? "" : "off";
  html += R"HTML("></span>)HTML";
  html += scannerName(d);
  html += R"HTML(</div><div class="muted">Printing stays on USB IF1 ALT0. The scanner service independently claims IF0 ALT1 for IPP-over-USB/eSCL traffic.</div></div><div class="card"><div class="grid"><div class="item"><div class="key">AirScan name</div><div class="value">HP Smart Tank 520/540 Scanner</div></div><div class="item"><div class="key">eSCL address</div><div class="value"><code>printer.local:8080/eSCL/</code></div></div><div class="item"><div class="key">Capabilities</div><div class="value"><a href="http://printer.local:8080/eSCL/ScannerCapabilities">Open XML</a></div></div><div class="item"><div class="key">Scanner status</div><div class="value"><a href="http://printer.local:8080/eSCL/ScannerStatus">Open XML</a></div></div></div><p class="muted">ScanBridge and other AirScan clients should discover the scanner through <code>_uscan._tcp</code> on port 8080. Capabilities and status are served by the ESP32; scan jobs and image data are forwarded through the HP USB scanner interface.</p></div></main></body></html>)HTML";
  configServer.send(200, "text/html; charset=utf-8", html);
}
} // namespace

void ensureUsbScannerWebRoutesInstalled() {
  if (routesInstalled) return;
  routesInstalled = true;
  configServer.on("/scan", HTTP_GET, sendScannerPage);
  configServer.on("/scan/status.json", HTTP_GET, sendScannerStatusJson);
  ensureUsbScannerBackendStarted();
  Serial.println("[SCAN] /scan registered; eSCL service starting on TCP 8080");
}
