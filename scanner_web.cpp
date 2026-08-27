#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include "usb_scanner_backend.h"

extern WebServer configServer;

namespace {
UsbScannerBackend scannerView;
bool routesInstalled = false;

String jsonEscape(String s) {
  s.replace("\\", "\\\\");
  s.replace("\"", "\\\"");
  s.replace("\r", "\\r");
  s.replace("\n", "\\n");
  return s;
}

String scannerHost() {
  if (WiFi.status() == WL_CONNECTED) return WiFi.localIP().toString();
  const wifi_mode_t mode = WiFi.getMode();
  if (mode == WIFI_AP || mode == WIFI_AP_STA) return WiFi.softAPIP().toString();
  return String();
}

String capabilitiesUrl() {
  const String host = scannerHost();
  if (!host.length()) return String();
  return String("http://") + host + ":" + String(UsbScannerBackend::NETWORK_PORT) +
         "/eSCL/ScannerCapabilities";
}

void sendScannerStatus() {
  const bool ready = scannerView.ready();
  const bool busy = scannerView.busy();
  String out;
  out.reserve(320);
  out += "{\"ready\":" + String(ready ? "true" : "false");
  out += ",\"busy\":" + String(busy ? "true" : "false");
  out += ",\"interface\":" + String(scannerView.interfaceNumber());
  out += ",\"alt\":" + String(scannerView.alternateSetting());
  out += ",\"bulkOut\":" + String(scannerView.bulkOutEndpoint());
  out += ",\"bulkIn\":" + String(scannerView.bulkInEndpoint());
  out += ",\"port\":" + String(UsbScannerBackend::NETWORK_PORT);
  out += ",\"capabilitiesUrl\":\"" + jsonEscape(capabilitiesUrl()) + "\"}";
  configServer.send(200, "application/json", out);
}

void sendScannerPage() {
  const bool ready = scannerView.ready();
  const bool busy = scannerView.busy();
  const String capUrl = capabilitiesUrl();

  String html;
  html.reserve(6200);
  html += R"HTML(<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>HP Scanner</title><style>
:root{color-scheme:light;--bg:#eef2f7;--card:rgba(255,255,255,.72);--text:#101114;--muted:#69707d;--blue:#007aff;--green:#34c759;--orange:#ff9500;--red:#ff3b30}*{box-sizing:border-box}body{margin:0;min-height:100vh;font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",system-ui,sans-serif;color:var(--text);background:linear-gradient(145deg,#eef2f7,#f7f9fc)}main{max-width:760px;margin:auto;padding:28px 16px 48px}.top{display:flex;justify-content:space-between;align-items:center;gap:12px;margin-bottom:18px}h1{font-size:28px;margin:0}.muted{color:var(--muted);font-size:13px;line-height:1.55}.card{background:var(--card);border:1px solid #fff;border-radius:22px;padding:20px;margin:12px 0;box-shadow:0 16px 45px rgba(30,45,70,.1)}.status{font-size:21px;font-weight:800;margin:7px 0}.dot{display:inline-block;width:9px;height:9px;border-radius:50%;background:var(--green);margin-right:8px}.dot.wait{background:var(--orange)}.dot.err{background:var(--red)}.grid{display:grid;grid-template-columns:1fr 1fr;gap:10px}.item{padding:13px;border-radius:15px;background:rgba(255,255,255,.55)}.key{font-size:11px;text-transform:uppercase;letter-spacing:.06em;color:var(--muted);font-weight:750}.value{margin-top:5px;font-weight:700;word-break:break-word}.btn{display:inline-block;text-decoration:none;border:0;border-radius:999px;padding:11px 16px;background:var(--blue);color:#fff;font-weight:700;font-size:14px}.btn.secondary{background:#fff;color:var(--text);border:1px solid #dde3eb}.actions{display:flex;gap:9px;flex-wrap:wrap;margin-top:14px}code{font-family:ui-monospace,SFMono-Regular,Consolas,monospace;font-size:12px;word-break:break-all}@media(max-width:560px){.grid{grid-template-columns:1fr}}</style></head><body><main><div class="top"><div><div class="muted">ESP32-S3 AirScan / eSCL</div><h1>HP Scanner</h1></div><a class="btn secondary" href="/">Printer dashboard</a></div><div class="card"><div class="key">Scanner status</div><div id="status" class="status"><span id="dot" class="dot )HTML";
  html += ready ? "" : "wait";
  html += R"HTML("></span><span id="statusText">)HTML";
  html += busy ? "Busy" : (ready ? "Ready" : "Waiting for USB scanner interface");
  html += R"HTML(</span></div><div class="muted">This page is the browser fallback/diagnostic view. Android AirScan apps should discover the scanner automatically through <code>_uscan._tcp</code>.</div></div><div class="card"><div class="grid"><div class="item"><div class="key">USB interface</div><div id="usbIf" class="value">)HTML";
  html += ready ? (String("IF ") + scannerView.interfaceNumber() + " · ALT " + scannerView.alternateSetting()) : "Not claimed";
  html += R"HTML(</div></div><div class="item"><div class="key">USB endpoints</div><div id="eps" class="value">)HTML";
  if (ready) {
    html += String("OUT 0x") + String(scannerView.bulkOutEndpoint(), HEX) + " · IN 0x" + String(scannerView.bulkInEndpoint(), HEX);
  } else {
    html += "Unavailable";
  }
  html += R"HTML(</div></div><div class="item"><div class="key">Wi-Fi scanner service</div><div class="value">_uscan._tcp · TCP )HTML";
  html += String(UsbScannerBackend::NETWORK_PORT);
  html += R"HTML(</div></div><div class="item"><div class="key">eSCL root</div><div class="value"><code>/eSCL</code></div></div></div><div class="actions"><a id="capBtn" class="btn" target="_blank" rel="noopener" href=")HTML";
  html += capUrl.length() ? capUrl : "#";
  html += R"HTML(">Test ScannerCapabilities</a></div><p class="muted">If the capabilities test returns XML, the Wi-Fi-to-USB eSCL path is responding. For normal scanning, use an AirScan/eSCL app such as ScanBridge; no manual USB interface selection is required.</p></div><script>
async function refresh(){try{const r=await fetch('/scan/status.json?ts='+Date.now());const s=await r.json();const t=document.getElementById('statusText'),d=document.getElementById('dot');t.textContent=s.busy?'Busy':(s.ready?'Ready':'Waiting for USB scanner interface');d.className='dot '+(s.ready?'':'wait');document.getElementById('usbIf').textContent=s.ready?'IF '+s.interface+' · ALT '+s.alt:'Not claimed';document.getElementById('eps').textContent=s.ready?'OUT 0x'+s.bulkOut.toString(16).padStart(2,'0')+' · IN 0x'+s.bulkIn.toString(16).padStart(2,'0'):'Unavailable';const b=document.getElementById('capBtn');b.href=s.capabilitiesUrl||'#';}catch(e){document.getElementById('statusText').textContent='Status unavailable';document.getElementById('dot').className='dot err';}}setInterval(refresh,2000);refresh();
</script></main></body></html>)HTML";

  configServer.send(200, "text/html; charset=utf-8", html);
}
} // namespace

void ensureUsbScannerWebRoutesInstalled() {
  if (routesInstalled) return;
  routesInstalled = true;
  configServer.on("/scan", HTTP_GET, sendScannerPage);
  configServer.on("/scan/status.json", HTTP_GET, sendScannerStatus);
  Serial.println("[SCAN] Browser fallback available at /scan");
}
