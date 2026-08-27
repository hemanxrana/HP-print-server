#include <Arduino.h>
#include <WebServer.h>

extern WebServer configServer;

namespace {
bool routesInstalled = false;

void sendScannerStatus() {
  configServer.send(200, "application/json",
                    "{\"mode\":\"web-only\",\"escl\":false,\"implemented\":false}");
}

void sendScannerPage() {
  String html;
  html.reserve(5200);
  html += R"HTML(<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>HP Scanner</title><style>
:root{color-scheme:light;--bg:#eef2f7;--card:rgba(255,255,255,.74);--text:#101114;--muted:#69707d;--blue:#007aff;--orange:#ff9500}*{box-sizing:border-box}body{margin:0;min-height:100vh;font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",system-ui,sans-serif;color:var(--text);background:linear-gradient(145deg,#eef2f7,#f7f9fc)}main{max-width:760px;margin:auto;padding:28px 16px 48px}.top{display:flex;justify-content:space-between;align-items:center;gap:12px;margin-bottom:18px}h1{font-size:28px;margin:0}.muted{color:var(--muted);font-size:13px;line-height:1.55}.card{background:var(--card);border:1px solid #fff;border-radius:22px;padding:20px;margin:12px 0;box-shadow:0 16px 45px rgba(30,45,70,.1)}.status{font-size:21px;font-weight:800;margin:7px 0}.dot{display:inline-block;width:9px;height:9px;border-radius:50%;background:var(--orange);margin-right:8px}.grid{display:grid;grid-template-columns:1fr 1fr;gap:10px}.item{padding:13px;border-radius:15px;background:rgba(255,255,255,.55)}.key{font-size:11px;text-transform:uppercase;letter-spacing:.06em;color:var(--muted);font-weight:750}.value{margin-top:5px;font-weight:700;word-break:break-word}.btn{display:inline-block;text-decoration:none;border-radius:999px;padding:11px 16px;background:#fff;color:var(--text);border:1px solid #dde3eb;font-weight:700;font-size:14px}code{font-family:ui-monospace,SFMono-Regular,Consolas,monospace;font-size:12px}@media(max-width:560px){.grid{grid-template-columns:1fr}}</style></head><body><main><div class="top"><div><div class="muted">ESP32-S3 scanner</div><h1>HP Scanner</h1></div><a class="btn" href="/">Printer dashboard</a></div><div class="card"><div class="key">Scanner mode</div><div class="status"><span class="dot"></span>Web scan page only</div><p class="muted">AirScan/eSCL discovery and the TCP 8080 IPP-over-USB proxy have been removed. They did not return scanner capabilities reliably on this printer.</p></div><div class="card"><div class="grid"><div class="item"><div class="key">Web entry point</div><div class="value"><code>/scan</code></div></div><div class="item"><div class="key">Printing</div><div class="value">Unchanged · RAW TCP 9100</div></div><div class="item"><div class="key">Known scanner-side USB function</div><div class="value">IF0 ALT0 · vendor-specific</div></div><div class="item"><div class="key">Known endpoints</div><div class="value">OUT 0x01 · IN 0x82 · INT IN 0x83</div></div></div><p class="muted">The next scanner implementation should talk to the HP vendor-specific interface directly and stream the resulting image through this page. Until that protocol is implemented, this page intentionally does not send experimental USB scan commands.</p></div></main></body></html>)HTML";
  configServer.send(200, "text/html; charset=utf-8", html);
}
} // namespace

void ensureUsbScannerWebRoutesInstalled() {
  if (routesInstalled) return;
  routesInstalled = true;
  configServer.on("/scan", HTTP_GET, sendScannerPage);
  configServer.on("/scan/status.json", HTTP_GET, sendScannerStatus);
  Serial.println("[SCAN] Web scanner page available at /scan (eSCL disabled)");
}
