#include <Arduino.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include "mdns.h"
#include "usb_host_manager.h"

extern WebServer configServer;
extern UsbHostManager usbHost;

namespace {
static constexpr const char *SCANNER_SERVICE_NAME = "HP Smart Tank 520/540 Scanner";
static constexpr const char *SCANNER_MODEL = "HP Smart Tank 520/540 series";
static constexpr const char *ESCL_VERSION = "2.62";
static constexpr uint16_t ESCL_PORT = 80;

bool routesInstalled = false;
bool discoveryAdvertised = false;
String scannerUuid;

bool knownSmartTankScanner(const UsbDeviceInfo &d) {
  return d.attached && d.vid == 0x03F0 && d.pid == 0x4554;
}

String xmlEscape(String s) {
  s.replace("&", "&amp;");
  s.replace("<", "&lt;");
  s.replace(">", "&gt;");
  s.replace("\"", "&quot;");
  s.replace("'", "&apos;");
  return s;
}

String stableScannerUuid() {
  if (scannerUuid.length()) return scannerUuid;

  const uint64_t id = ESP.getEfuseMac();
  char value[37];
  snprintf(value, sizeof(value),
           "52054000-%04x-4%03x-8%03x-%012llx",
           (unsigned)((id >> 32) & 0xFFFF),
           (unsigned)((id >> 20) & 0x0FFF),
           (unsigned)((id >> 8) & 0x0FFF),
           (unsigned long long)(id & 0xFFFFFFFFFFFFULL));
  scannerUuid = value;
  return scannerUuid;
}

String scannerSerial(const UsbDeviceInfo &d) {
  if (d.serial.length()) return d.serial;
  return "03F0-4554-ESP32";
}

String scannerCapabilitiesXml() {
  const UsbDeviceInfo &d = usbHost.device();
  const String serial = xmlEscape(scannerSerial(d));
  const String uuid = stableScannerUuid();

  String xml;
  xml.reserve(4200);
  xml += F("<?xml version=\"1.0\" encoding=\"UTF-8\"?>");
  xml += F("<scan:ScannerCapabilities xmlns:scan=\"http://schemas.hp.com/imaging/escl/2011/05/03\" xmlns:pwg=\"http://www.pwg.org/schemas/2010/12/sm\">");
  xml += F("<pwg:Version>2.62</pwg:Version>");
  xml += F("<pwg:MakeAndModel>HP Smart Tank 520/540 series</pwg:MakeAndModel>");
  xml += F("<scan:Manufacturer>HP</scan:Manufacturer>");
  xml += F("<pwg:SerialNumber>"); xml += serial; xml += F("</pwg:SerialNumber>");
  xml += F("<scan:UUID>"); xml += uuid; xml += F("</scan:UUID>");
  xml += F("<scan:AdminURI>http://printer.local/scan</scan:AdminURI>");
  xml += F("<scan:Platen><scan:PlatenInputCaps>");
  xml += F("<scan:MinWidth>16</scan:MinWidth><scan:MaxWidth>2550</scan:MaxWidth>");
  xml += F("<scan:MinHeight>16</scan:MinHeight><scan:MaxHeight>3508</scan:MaxHeight>");
  xml += F("<scan:MaxScanRegions>1</scan:MaxScanRegions>");
  xml += F("<scan:SettingProfiles><scan:SettingProfile>");
  xml += F("<scan:ColorModes><scan:ColorMode>Grayscale8</scan:ColorMode><scan:ColorMode>RGB24</scan:ColorMode></scan:ColorModes>");
  xml += F("<scan:DocumentFormats><pwg:DocumentFormat>image/jpeg</pwg:DocumentFormat><scan:DocumentFormatExt>image/jpeg</scan:DocumentFormatExt></scan:DocumentFormats>");
  xml += F("<scan:SupportedResolutions><scan:DiscreteResolutions>");
  xml += F("<scan:DiscreteResolution><scan:XResolution>150</scan:XResolution><scan:YResolution>150</scan:YResolution></scan:DiscreteResolution>");
  xml += F("<scan:DiscreteResolution><scan:XResolution>300</scan:XResolution><scan:YResolution>300</scan:YResolution></scan:DiscreteResolution>");
  xml += F("<scan:DiscreteResolution><scan:XResolution>600</scan:XResolution><scan:YResolution>600</scan:YResolution></scan:DiscreteResolution>");
  xml += F("<scan:DiscreteResolution><scan:XResolution>1200</scan:XResolution><scan:YResolution>1200</scan:YResolution></scan:DiscreteResolution>");
  xml += F("</scan:DiscreteResolutions></scan:SupportedResolutions>");
  xml += F("<scan:ColorSpaces><scan:ColorSpace>sRGB</scan:ColorSpace></scan:ColorSpaces>");
  xml += F("</scan:SettingProfile></scan:SettingProfiles>");
  xml += F("<scan:SupportedIntents><scan:Intent>Document</scan:Intent><scan:Intent>Photo</scan:Intent><scan:Intent>Preview</scan:Intent></scan:SupportedIntents>");
  xml += F("<scan:EdgeAutoDetection><scan:SupportedEdge>TopEdge</scan:SupportedEdge><scan:SupportedEdge>LeftEdge</scan:SupportedEdge><scan:SupportedEdge>BottomEdge</scan:SupportedEdge><scan:SupportedEdge>RightEdge</scan:SupportedEdge></scan:EdgeAutoDetection>");
  xml += F("<scan:MaxOpticalXResolution>1200</scan:MaxOpticalXResolution><scan:MaxOpticalYResolution>1200</scan:MaxOpticalYResolution>");
  xml += F("<scan:RiskyLeftMargin>0</scan:RiskyLeftMargin><scan:RiskyRightMargin>0</scan:RiskyRightMargin><scan:RiskyTopMargin>0</scan:RiskyTopMargin><scan:RiskyBottomMargin>0</scan:RiskyBottomMargin>");
  xml += F("<scan:MaxPhysicalWidth>2550</scan:MaxPhysicalWidth><scan:MaxPhysicalHeight>3508</scan:MaxPhysicalHeight>");
  xml += F("</scan:PlatenInputCaps></scan:Platen></scan:ScannerCapabilities>");
  return xml;
}

String scannerStatusXml() {
  const bool attached = knownSmartTankScanner(usbHost.device());
  String xml;
  xml.reserve(420);
  xml += F("<?xml version=\"1.0\" encoding=\"UTF-8\"?>");
  xml += F("<scan:ScannerStatus xmlns:scan=\"http://schemas.hp.com/imaging/escl/2011/05/03\" xmlns:pwg=\"http://www.pwg.org/schemas/2010/12/sm\">");
  xml += F("<pwg:Version>2.62</pwg:Version><pwg:State>");
  xml += attached ? F("Idle") : F("Down");
  xml += F("</pwg:State><scan:Jobs/></scan:ScannerStatus>");
  return xml;
}

void sendXml(const String &xml) {
  configServer.sendHeader("Cache-Control", "no-cache");
  configServer.send(200, "text/xml; charset=utf-8", xml);
}

void sendScannerCapabilities() {
  sendXml(scannerCapabilitiesXml());
  Serial.println("[eSCL] ScannerCapabilities served locally");
}

void sendEsclScannerStatus() {
  sendXml(scannerStatusXml());
}

void rejectScanJobUntilUsbTransportReady() {
  configServer.sendHeader("Retry-After", "5");
  configServer.send(503, "text/plain; charset=utf-8",
                    "Scanner discovered and capabilities are available, but the USB scan-job transport is not enabled yet.");
  Serial.println("[eSCL] ScanJobs requested; USB scan-job transport is not enabled yet");
}

void advertiseEsclScanner() {
  if (discoveryAdvertised) return;
  if (!MDNS.addService("uscan", "tcp", ESCL_PORT)) {
    Serial.println("[eSCL] Could not advertise _uscan._tcp");
    return;
  }

  const String uuid = stableScannerUuid();
  MDNS.addServiceTxt("uscan", "tcp", "txtvers", "1");
  MDNS.addServiceTxt("uscan", "tcp", "rs", "eSCL");
  MDNS.addServiceTxt("uscan", "tcp", "ty", SCANNER_MODEL);
  MDNS.addServiceTxt("uscan", "tcp", "pdl", "image/jpeg");
  MDNS.addServiceTxt("uscan", "tcp", "cs", "color,grayscale");
  MDNS.addServiceTxt("uscan", "tcp", "is", "platen");
  MDNS.addServiceTxt("uscan", "tcp", "duplex", "F");
  MDNS.addServiceTxt("uscan", "tcp", "vers", ESCL_VERSION);
  MDNS.addServiceTxt("uscan", "tcp", "UUID", uuid.c_str());
  MDNS.addServiceTxt("uscan", "tcp", "adminurl", "http://printer.local/scan");

  const esp_err_t instanceResult =
      mdns_service_instance_name_set("_uscan", "_tcp", SCANNER_SERVICE_NAME);
  if (instanceResult != ESP_OK) {
    Serial.printf("[eSCL] Scanner service instance name failed: %s\n",
                  esp_err_to_name(instanceResult));
  }

  discoveryAdvertised = true;
  Serial.printf("[eSCL] AirScan discovery: %s._uscan._tcp port %u rs=eSCL\n",
                SCANNER_SERVICE_NAME, ESCL_PORT);
}

void sendScannerStatusJson() {
  const UsbDeviceInfo &d = usbHost.device();
  const bool detected = knownSmartTankScanner(d);
  String out;
  out.reserve(520);
  out += "{\"detected\":" + String(detected ? "true" : "false");
  out += ",\"escl\":true,\"capabilities\":true,\"scanTransport\":false";
  out += ",\"name\":\"HP Smart Tank 520/540 Scanner\"";
  out += ",\"model\":\"HP Smart Tank 520/540 series\"";
  out += ",\"root\":\"/eSCL\",\"port\":80";
  out += ",\"uuid\":\"" + stableScannerUuid() + "\"";
  out += ",\"vid\":" + String(d.vid) + ",\"pid\":" + String(d.pid);
  out += ",\"interface\":0,\"alt\":0,\"bulkOut\":1,\"bulkIn\":130,\"interruptIn\":131}";
  configServer.send(200, "application/json", out);
}

void sendScannerPage() {
  const UsbDeviceInfo &d = usbHost.device();
  const bool detected = knownSmartTankScanner(d);
  String html;
  html.reserve(4600);
  html += R"HTML(<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><meta name="theme-color" content="#f3f5f7"><title>Scanner</title><style>
*{box-sizing:border-box}body{margin:0;background:#f3f5f7;color:#344054;font-family:system-ui,-apple-system,"Segoe UI",Roboto,Arial,sans-serif;font-size:14px;line-height:1.5}main{max-width:720px;margin:auto;padding:24px 16px 40px}.top{display:flex;align-items:center;justify-content:space-between;gap:12px;margin-bottom:18px}h1{margin:0;font-size:25px;font-weight:650;letter-spacing:-.02em}.sub,.muted{color:#758195;font-size:13px}.card{background:#fbfcfd;border:1px solid #e1e7ec;border-radius:14px;padding:18px;margin-top:12px}.status{font-size:18px;font-weight:650;margin:5px 0 2px}.dot{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:8px;background:#6f9d7d}.dot.off{background:#b58a62}.grid{display:grid;grid-template-columns:1fr 1fr;gap:9px;margin-top:14px}.item{background:#f6f8fa;border:1px solid #e7ebef;border-radius:10px;padding:11px}.key{font-size:11px;color:#7b8797;text-transform:uppercase;letter-spacing:.045em}.value{margin-top:3px;font-weight:600;word-break:break-word}.btn{display:inline-block;text-decoration:none;color:#466681;background:#edf3f7;border:1px solid #d8e1e8;border-radius:9px;padding:8px 12px;font-weight:600}code{font-family:ui-monospace,SFMono-Regular,Consolas,monospace;font-size:12px}@media(max-width:560px){.grid{grid-template-columns:1fr}}</style></head><body><main><div class="top"><div><h1>HP Smart Tank Scanner</h1><div class="sub">eSCL / AirScan compatibility service</div></div><a class="btn" href="/">Dashboard</a></div><div class="card"><div class="key">Scanner</div><div class="status"><span class="dot )HTML";
  html += detected ? "" : "off";
  html += R"HTML("></span>HP Smart Tank 520/540 Scanner</div><div class="muted">AirScan discovery and local eSCL capabilities are enabled. Scanner identity no longer depends on a USB capabilities request.</div></div><div class="card"><div class="grid"><div class="item"><div class="key">USB device</div><div class="value">)HTML";
  if (d.attached) html += String("VID 0x") + String(d.vid, HEX) + " · PID 0x" + String(d.pid, HEX);
  else html += "Not attached";
  html += R"HTML(</div></div><div class="item"><div class="key">AirScan service</div><div class="value">HP Smart Tank 520/540 Scanner</div></div><div class="item"><div class="key">eSCL root</div><div class="value"><code>http://printer.local/eSCL/</code></div></div><div class="item"><div class="key">Capabilities</div><div class="value"><a href="/eSCL/ScannerCapabilities">ScannerCapabilities</a></div></div></div><p class="muted">Discovery, identity, capabilities and scanner status are implemented locally for app compatibility. The remaining step is connecting eSCL ScanJobs to the HP USB scan-data transport; until then a scan attempt returns Service Unavailable instead of pretending that a scan succeeded.</p></div></main></body></html>)HTML";
  configServer.send(200, "text/html; charset=utf-8", html);
}
} // namespace

void ensureUsbScannerWebRoutesInstalled() {
  if (routesInstalled) return;
  routesInstalled = true;

  configServer.on("/scan", HTTP_GET, sendScannerPage);
  configServer.on("/scan/status.json", HTTP_GET, sendScannerStatusJson);
  configServer.on("/eSCL/ScannerCapabilities", HTTP_GET, sendScannerCapabilities);
  configServer.on("/eSCL/ScannerStatus", HTTP_GET, sendEsclScannerStatus);
  configServer.on("/eSCL/ScanJobs", HTTP_POST, rejectScanJobUntilUsbTransportReady);

  advertiseEsclScanner();
  Serial.println("[eSCL] Local AirScan capabilities/status routes registered");
}
