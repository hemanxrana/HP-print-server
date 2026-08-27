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

String scannerName(const UsbDeviceInfo &d) {
  if (!knownSmartTankScanner(d)) return "No scanner detected";
  if (d.product.length()) return d.product + " scanner";
  return "HP Smart Tank 520/540 scanner";
}

void sendScannerStatus() {
  const UsbDeviceInfo &d = usbHost.device();
  const bool detected = knownSmartTankScanner(d);
  String out;
  out.reserve(360);
  out += "{\"detected\":" + String(detected ? "true" : "false");
  out += ",\"implemented\":false";
  out += ",\"escl\":false";
  out += ",\"name\":\"" + scannerName(d) + "\"";
  out += ",\"vid\":" + String(d.vid);
  out += ",\"pid\":" + String(d.pid);
  out += ",\"interface\":0,\"alt\":0";
  out += ",\"class\":255,\"subclass\":204,\"protocol\":0";
  out += ",\"bulkOut\":1,\"bulkIn\":130,\"interruptIn\":131}";
  configServer.send(200, "application/json", out);
}

void sendScannerPage() {
  const UsbDeviceInfo &d = usbHost.device();
  const bool detected = knownSmartTankScanner(d);
  const String name = scannerName(d);

  String html;
  html.reserve(4200);
  html += R"HTML(<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><meta name="theme-color" content="#f3f5f7"><title>Scanner</title><style>
*{box-sizing:border-box}body{margin:0;background:#f3f5f7;color:#344054;font-family:system-ui,-apple-system,"Segoe UI",Roboto,Arial,sans-serif;font-size:14px;line-height:1.5}main{max-width:720px;margin:auto;padding:24px 16px 40px}.top{display:flex;align-items:center;justify-content:space-between;gap:12px;margin-bottom:18px}h1{margin:0;font-size:25px;font-weight:650;letter-spacing:-.02em}.sub{margin-top:4px;color:#758195;font-size:13px}.card{background:#fbfcfd;border:1px solid #e1e7ec;border-radius:14px;padding:18px;margin-top:12px}.status{font-size:18px;font-weight:650;margin:5px 0 2px}.dot{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:8px;background:#6f9d7d}.dot.off{background:#b58a62}.muted{color:#758195;font-size:13px}.grid{display:grid;grid-template-columns:1fr 1fr;gap:9px;margin-top:14px}.item{background:#f6f8fa;border:1px solid #e7ebef;border-radius:10px;padding:11px}.key{font-size:11px;color:#7b8797;text-transform:uppercase;letter-spacing:.045em}.value{margin-top:3px;font-weight:600}.btn{display:inline-block;text-decoration:none;color:#466681;background:#edf3f7;border:1px solid #d8e1e8;border-radius:9px;padding:8px 12px;font-weight:600}code{font-family:ui-monospace,SFMono-Regular,Consolas,monospace;font-size:12px}@media(max-width:560px){.grid{grid-template-columns:1fr}}</style></head><body><main><div class="top"><div><h1>Scanner</h1><div class="sub">USB scanner status and future browser scanning</div></div><a class="btn" href="/">Dashboard</a></div><div class="card"><div class="key">Detected device</div><div class="status"><span class="dot )HTML";
  html += detected ? "" : "off";
  html += R"HTML("></span>)HTML";
  html += name;
  html += R"HTML(</div><div class="muted">Detection uses the attached USB device identity, not eSCL capabilities. eSCL and its port 8080 proxy are disabled.</div></div><div class="card"><div class="grid"><div class="item"><div class="key">USB identity</div><div class="value">)HTML";
  if (d.attached) {
    html += String("VID 0x") + String(d.vid, HEX) + " · PID 0x" + String(d.pid, HEX);
  } else {
    html += "No USB device";
  }
  html += R"HTML(</div></div><div class="item"><div class="key">Scanner interface</div><div class="value">IF0 ALT0 · FF/CC/00</div></div><div class="item"><div class="key">Bulk endpoints</div><div class="value">OUT 0x01 · IN 0x82</div></div><div class="item"><div class="key">Status endpoint</div><div class="value">INT IN 0x83</div></div></div><p class="muted">The scanner interface is identified for this Smart Tank model, but actual scan commands are not enabled yet. The next step is implementing HP's vendor-specific IF0 protocol and streaming the resulting image through this page.</p></div></main></body></html>)HTML";
  configServer.send(200, "text/html; charset=utf-8", html);
}
} // namespace

void ensureUsbScannerWebRoutesInstalled() {
  if (routesInstalled) return;
  routesInstalled = true;
  configServer.on("/scan", HTTP_GET, sendScannerPage);
  configServer.on("/scan/status.json", HTTP_GET, sendScannerStatus);
  Serial.println("[SCAN] /scan registered; eSCL disabled; Smart Tank scanner identified by USB VID/PID");
}
