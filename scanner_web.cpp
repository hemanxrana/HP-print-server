#include <Arduino.h>
#include <WebServer.h>
#include "usb_host_manager.h"

extern WebServer configServer;
extern UsbHostManager usbHost;

namespace {
bool routesInstalled = false;

bool knownSmartTankScanner(const UsbDeviceInfo &d) {
  return d.attached && d.vid == 0x03F0 && d.pid == 0x4554;
}

void sendScannerStatusJson() {
  const UsbDeviceInfo &d = usbHost.device();
  const bool detected = knownSmartTankScanner(d);
  String out;
  out.reserve(220);
  out += "{\"detected\":" + String(detected ? "true" : "false");
  out += ",\"enabled\":false,\"status\":\"disabled\"}";
  configServer.send(200, "application/json", out);
}

void sendScannerPage() {
  const bool detected = knownSmartTankScanner(usbHost.device());
  String html;
  html.reserve(2600);
  html += R"HTML(<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Scanner</title><style>
*{box-sizing:border-box}body{margin:0;background:#f3f5f7;color:#344054;font-family:system-ui,-apple-system,"Segoe UI",Roboto,Arial,sans-serif;font-size:14px;line-height:1.5}main{max-width:720px;margin:auto;padding:24px 16px 40px}.top{display:flex;align-items:center;justify-content:space-between;gap:12px;margin-bottom:18px}h1{margin:0;font-size:25px}.sub,.muted{color:#758195;font-size:13px}.card{background:#fbfcfd;border:1px solid #e1e7ec;border-radius:14px;padding:18px;margin-top:12px}.status{font-size:18px;font-weight:650;margin:5px 0 2px}.btn{display:inline-block;text-decoration:none;color:#466681;background:#edf3f7;border:1px solid #d8e1e8;border-radius:9px;padding:8px 12px;font-weight:600}</style></head><body><main><div class="top"><div><h1>Scanner</h1><div class="sub">Temporarily disabled</div></div><a class="btn" href="/">Dashboard</a></div><div class="card"><div class="status">Scanner USB interface is not active</div><p class="muted">The scanner backend is intentionally not started in this build so it cannot claim or switch any USB scanner interface while printing is being validated.</p><p class="muted">USB multifunction device detected: )HTML";
  html += detected ? "yes" : "no";
  html += R"HTML(.</p></div></main></body></html>)HTML";
  configServer.send(200, "text/html; charset=utf-8", html);
}
} // namespace

void ensureUsbScannerWebRoutesInstalled() {
  if (routesInstalled) return;
  routesInstalled = true;
  configServer.on("/scan", HTTP_GET, sendScannerPage);
  configServer.on("/scan/status.json", HTTP_GET, sendScannerStatusJson);
  Serial.println("[SCAN] Scanner routes registered; USB scanner backend DISABLED");
}
