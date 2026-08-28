from pathlib import Path

p = Path('experiments/android_print_probe/android_print_probe.ino')
s = p.read_text()

def once(old, new, label):
    global s
    if old not in s:
        raise SystemExit(f'{label}: marker not found')
    s = s.replace(old, new, 1)

once(
'''enum ProbeMode : uint8_t { MODE_SAFE = 0, MODE_CLASSIC_RAW = 1, MODE_IPP_USB = 2 };
ProbeMode probeMode = MODE_SAFE;
uint32_t jobSequence = 0;
size_t minLoopStackFree = (size_t)-1;
''',
'''enum ProbeMode : uint8_t { MODE_SAFE = 0, MODE_CLASSIC_RAW = 1, MODE_IPP_USB = 2 };
ProbeMode probeMode = MODE_SAFE;

enum AdvertProfile : uint8_t {
  ADV_HP_INKJET_BROAD = 0,
  ADV_PCL3GUI_ONLY,
  ADV_PCL3GUI_PREFERRED,
  ADV_PCLM_ONLY,
  ADV_URF_ONLY,
  ADV_PWG_ONLY,
  ADV_JPEG_ONLY,
  ADV_AUTOMATIC_ONLY,
  ADV_PDF_EXPERIMENTAL
};
AdvertProfile advertProfile = ADV_HP_INKJET_BROAD;
bool mdnsRefreshPending = false;
uint32_t jobSequence = 0;
size_t minLoopStackFree = (size_t)-1;
''', 'advert enum')

once(
'''const char *modeName() {
  switch (probeMode) {
    case MODE_SAFE: return "SAFE CAPTURE";
    case MODE_CLASSIC_RAW: return "CLASSIC USB RAW";
    case MODE_IPP_USB: return "IPP-OVER-USB EXPERIMENTAL";
  }
  return "UNKNOWN";
}
''',
'''const char *modeName() {
  switch (probeMode) {
    case MODE_SAFE: return "SAFE CAPTURE";
    case MODE_CLASSIC_RAW: return "CLASSIC USB RAW";
    case MODE_IPP_USB: return "IPP-OVER-USB EXPERIMENTAL";
  }
  return "UNKNOWN";
}

const char *advertProfileName() {
  switch (advertProfile) {
    case ADV_HP_INKJET_BROAD: return "HP INKJET BROAD";
    case ADV_PCL3GUI_ONLY: return "PCL3GUI ONLY";
    case ADV_PCL3GUI_PREFERRED: return "PCL3GUI PREFERRED";
    case ADV_PCLM_ONLY: return "PCLM ONLY";
    case ADV_URF_ONLY: return "APPLE RASTER / URF ONLY";
    case ADV_PWG_ONLY: return "PWG RASTER ONLY";
    case ADV_JPEG_ONLY: return "JPEG ONLY";
    case ADV_AUTOMATIC_ONLY: return "AUTOMATIC / OCTET-STREAM ONLY";
    case ADV_PDF_EXPERIMENTAL: return "PDF EXPERIMENTAL";
  }
  return "UNKNOWN";
}

String advertisedPdlList() {
  switch (advertProfile) {
    case ADV_PCL3GUI_ONLY: return "application/vnd.hp-PCL";
    case ADV_PCL3GUI_PREFERRED: return "application/vnd.hp-PCL,application/octet-stream,application/PCLm,image/jpeg,image/urf,image/pwg-raster";
    case ADV_PCLM_ONLY: return "application/PCLm";
    case ADV_URF_ONLY: return "image/urf";
    case ADV_PWG_ONLY: return "image/pwg-raster";
    case ADV_JPEG_ONLY: return "image/jpeg";
    case ADV_AUTOMATIC_ONLY: return "application/octet-stream";
    case ADV_PDF_EXPERIMENTAL: return "application/pdf";
    case ADV_HP_INKJET_BROAD:
    default: return "application/vnd.hp-PCL,image/jpeg,image/urf,image/pwg-raster,application/PCLm,application/octet-stream";
  }
}

String advertisedVersionList() {
  switch (advertProfile) {
    case ADV_PCL3GUI_ONLY: return "PCL3GUI,PCL3,PJL,Automatic";
    case ADV_PCL3GUI_PREFERRED: return "PCL3GUI,PCL3,PJL,Automatic,PCLM,JPEG,AppleRaster,PWGRaster";
    case ADV_PCLM_ONLY: return "PCLM";
    case ADV_URF_ONLY: return "AppleRaster";
    case ADV_PWG_ONLY: return "PWGRaster";
    case ADV_JPEG_ONLY: return "JPEG";
    case ADV_AUTOMATIC_ONLY: return "Automatic";
    case ADV_PDF_EXPERIMENTAL: return "PDF";
    case ADV_HP_INKJET_BROAD:
    default: return "PCL3GUI,PCL3,PJL,Automatic,JPEG,AppleRaster,PWGRaster,PCLM";
  }
}

const char *advertisedDefaultFormat() {
  switch (advertProfile) {
    case ADV_PCL3GUI_ONLY:
    case ADV_PCL3GUI_PREFERRED: return "application/vnd.hp-PCL";
    case ADV_PCLM_ONLY: return "application/PCLm";
    case ADV_URF_ONLY: return "image/urf";
    case ADV_PWG_ONLY: return "image/pwg-raster";
    case ADV_JPEG_ONLY: return "image/jpeg";
    case ADV_PDF_EXPERIMENTAL: return "application/pdf";
    case ADV_AUTOMATIC_ONLY:
    case ADV_HP_INKJET_BROAD:
    default: return "application/octet-stream";
  }
}
''', 'advert helpers')

once(
'''void moreString(IppWriter &w, uint8_t tag, const char *v) { w.str(tag, nullptr, v); }
void moreInteger(IppWriter &w, uint8_t tag, int32_t v) { w.integer(tag, nullptr, v); }
''',
'''void moreString(IppWriter &w, uint8_t tag, const char *v) { w.str(tag, nullptr, v); }
void moreInteger(IppWriter &w, uint8_t tag, int32_t v) { w.integer(tag, nullptr, v); }

void addAdvertisedDocumentFormats(IppWriter &w) {
  auto add = [&](const char *name, bool first) {
    if (first) w.str(0x49, "document-format-supported", name);
    else moreString(w, 0x49, name);
  };

  bool first = true;
  auto put = [&](const char *name) { add(name, first); first = false; };
  switch (advertProfile) {
    case ADV_PCL3GUI_ONLY: put("application/vnd.hp-PCL"); break;
    case ADV_PCL3GUI_PREFERRED:
      put("application/vnd.hp-PCL"); put("application/octet-stream"); put("application/PCLm");
      put("image/jpeg"); put("image/urf"); put("image/pwg-raster"); break;
    case ADV_PCLM_ONLY: put("application/PCLm"); break;
    case ADV_URF_ONLY: put("image/urf"); break;
    case ADV_PWG_ONLY: put("image/pwg-raster"); break;
    case ADV_JPEG_ONLY: put("image/jpeg"); break;
    case ADV_AUTOMATIC_ONLY: put("application/octet-stream"); break;
    case ADV_PDF_EXPERIMENTAL: put("application/pdf"); break;
    case ADV_HP_INKJET_BROAD:
    default:
      put("application/vnd.hp-PCL"); put("image/jpeg"); put("image/urf");
      put("image/pwg-raster"); put("application/PCLm"); put("application/octet-stream"); break;
  }
  w.str(0x49, "document-format-default", advertisedDefaultFormat());

  const String versions = advertisedVersionList();
  int start = 0;
  bool firstVersion = true;
  while (start < versions.length()) {
    int comma = versions.indexOf(',', start);
    if (comma < 0) comma = versions.length();
    const String value = versions.substring(start, comma);
    if (firstVersion) w.str(0x41, "document-format-version-supported", value.c_str());
    else moreString(w, 0x41, value.c_str());
    firstVersion = false;
    start = comma + 1;
  }
}
''', 'format writer helper')

once(
'''  w.str(0x49, "document-format-supported", "application/PCLm");
  moreString(w, 0x49, "application/pdf"); moreString(w, 0x49, "image/jpeg");
  w.str(0x49, "document-format-default", "application/PCLm");
''',
'''  addAdvertisedDocumentFormats(w);
  w.str(0x44, "pdl-override-supported", "attempted");
''', 'replace static formats')

once(
'''void handleMode() {
''',
'''void handleAdvertProfile() {
  if (!web.hasArg("profile")) { web.send(400, "text/plain", "Missing profile"); return; }
  const String profile = web.arg("profile");
  if (profile == "hp-broad") advertProfile = ADV_HP_INKJET_BROAD;
  else if (profile == "pcl3gui") advertProfile = ADV_PCL3GUI_ONLY;
  else if (profile == "pcl3gui-preferred") advertProfile = ADV_PCL3GUI_PREFERRED;
  else if (profile == "pclm") advertProfile = ADV_PCLM_ONLY;
  else if (profile == "urf") advertProfile = ADV_URF_ONLY;
  else if (profile == "pwg") advertProfile = ADV_PWG_ONLY;
  else if (profile == "jpeg") advertProfile = ADV_JPEG_ONLY;
  else if (profile == "auto") advertProfile = ADV_AUTOMATIC_ONLY;
  else if (profile == "pdf") advertProfile = ADV_PDF_EXPERIMENTAL;
  else { web.send(400, "text/plain", "Invalid profile"); return; }
  mdnsRefreshPending = true;
  Serial.printf("[PROBE][PDL] Advertisement changed to %s default=%s pdl=%s versions=%s\\n",
                advertProfileName(), advertisedDefaultFormat(), advertisedPdlList().c_str(),
                advertisedVersionList().c_str());
  web.sendHeader("Location", "/"); web.send(303);
}

void handleMode() {
''', 'pdl handler')

once(
'''  html += "<h1>Android Print Probe — one flash</h1>";
  html += "<section><h2>Recommended next action</h2><div class='good'>" + htmlEscape(recommendedAction()) + "</div></section>";

  html += "<section><h2>1. Test Mode</h2><p><b>Current: " + String(modeName()) + "</b></p><form method='POST' action='/mode'>";
''',
'''  html += "<h1>Android Print Probe — one flash</h1>";
  html += "<section><h2>Recommended next action</h2><div class='good'>" + htmlEscape(recommendedAction()) + "</div></section>";

  html += "<section><h2>1. Printer language advertisement</h2><p><b>Current: " + String(advertProfileName()) + "</b></p>";
  html += "<table><tr><th>Default MIME</th><td><code>" + String(advertisedDefaultFormat()) + "</code></td></tr>";
  html += "<tr><th>Supported MIME</th><td><code>" + htmlEscape(advertisedPdlList()) + "</code></td></tr>";
  html += "<tr><th>Versions / commands</th><td><code>" + htmlEscape(advertisedVersionList()) + "</code></td></tr></table>";
  html += "<form method='POST' action='/pdl'>";
  html += "<label><input type='radio' name='profile' value='hp-broad' " + String(advertProfile == ADV_HP_INKJET_BROAD ? "checked" : "") + "> HP inkjet broad — PCL3GUI + PCL3 + PJL + PCLm + URF + PWG + JPEG + Automatic</label>";
  html += "<label><input type='radio' name='profile' value='pcl3gui' " + String(advertProfile == ADV_PCL3GUI_ONLY ? "checked" : "") + "> PCL3GUI only — <code>application/vnd.hp-PCL</code>; strongest test to make HP Print Service render HP PCL</label>";
  html += "<label><input type='radio' name='profile' value='pcl3gui-preferred' " + String(advertProfile == ADV_PCL3GUI_PREFERRED ? "checked" : "") + "> PCL3GUI preferred — HP PCL first/default, but keep mobile fallbacks</label>";
  html += "<label><input type='radio' name='profile' value='pclm' " + String(advertProfile == ADV_PCLM_ONLY ? "checked" : "") + "> PCLm only — <code>application/PCLm</code></label>";
  html += "<label><input type='radio' name='profile' value='urf' " + String(advertProfile == ADV_URF_ONLY ? "checked" : "") + "> Apple Raster / URF only — <code>image/urf</code></label>";
  html += "<label><input type='radio' name='profile' value='pwg' " + String(advertProfile == ADV_PWG_ONLY ? "checked" : "") + "> PWG Raster only — <code>image/pwg-raster</code></label>";
  html += "<label><input type='radio' name='profile' value='jpeg' " + String(advertProfile == ADV_JPEG_ONLY ? "checked" : "") + "> JPEG only — <code>image/jpeg</code></label>";
  html += "<label><input type='radio' name='profile' value='auto' " + String(advertProfile == ADV_AUTOMATIC_ONLY ? "checked" : "") + "> Automatic only — <code>application/octet-stream</code></label>";
  html += "<label><input type='radio' name='profile' value='pdf' " + String(advertProfile == ADV_PDF_EXPERIMENTAL ? "checked" : "") + "> PDF experimental — <code>application/pdf</code> (not part of the Smart Tank-style profile)</label>";
  html += "<button>Apply advertisement</button></form><p class='warn'>Changing this updates IPP attributes immediately and refreshes mDNS. Android may cache capabilities; reopen the print dialog or remove/re-add the printer if the format does not change.</p></section>";

  html += "<section><h2>2. Test Mode</h2><p><b>Current: " + String(modeName()) + "</b></p><form method='POST' action='/mode'>";
''', 'dashboard pdl section')

once('''  html += "<section><h2>2. USB printer status</h2><table><tr><th>State</th><td>" + htmlEscape(usbStateText()) + "</td></tr>";
''', '''  html += "<section><h2>3. USB printer status</h2><table><tr><th>State</th><td>" + htmlEscape(usbStateText()) + "</td></tr>";
''', 'renumber usb')
once('''  html += "<section><h2>3. IPP-over-USB interface selector</h2>";
''', '''  html += "<section><h2>4. IPP-over-USB interface selector</h2>";
''', 'renumber ippusb')
once('''  html += "<section><h2>4. Last Android request</h2>" + lastRequestHtml() + "</section>";
  html += "<section><h2>5. Captured document / transport result</h2>" + jobSummaryHtml();
''', '''  html += "<section><h2>5. Last Android request</h2>" + lastRequestHtml() + "</section>";
  html += "<section><h2>6. Captured document / transport result</h2>" + jobSummaryHtml();
''', 'renumber request result')
once('''  html += "<section><h2>6. Memory / stability</h2><table>";
''', '''  html += "<section><h2>7. Memory / stability</h2><table>";
''', 'renumber memory')

once(
'''  MDNS.addServiceTxt("ipp", "tcp", "note", "ESP32 one-flash Android print probe");
  MDNS.addServiceTxt("ipp", "tcp", "pdl", "application/PCLm,application/pdf,image/jpeg");
  MDNS.addService("pdl-datastream", "tcp", RAW_PORT);
''',
'''  MDNS.addServiceTxt("ipp", "tcp", "note", "ESP32 one-flash Android print probe");
  const String pdl = advertisedPdlList();
  const String cmd = advertisedVersionList();
  MDNS.addServiceTxt("ipp", "tcp", "pdl", pdl.c_str());
  MDNS.addServiceTxt("ipp", "tcp", "usb_MFG", "HP");
  MDNS.addServiceTxt("ipp", "tcp", "usb_MDL", MODEL);
  MDNS.addServiceTxt("ipp", "tcp", "usb_CMD", cmd.c_str());
  MDNS.addService("pdl-datastream", "tcp", RAW_PORT);
''', 'dynamic mdns pdl')

once(
'''  web.on("/", HTTP_GET, handleWebRoot);
  web.on("/mode", HTTP_POST, handleMode);
''',
'''  web.on("/", HTTP_GET, handleWebRoot);
  web.on("/pdl", HTTP_POST, handleAdvertProfile);
  web.on("/mode", HTTP_POST, handleMode);
''', 'pdl route')

once(
'''  web.handleClient();
  usbHost.poll();
''',
'''  web.handleClient();
  if (mdnsRefreshPending) {
    mdnsRefreshPending = false;
    advertiseProbe();
    Serial.printf("[PROBE][PDL] mDNS refreshed for %s\\n", advertProfileName());
  }
  usbHost.poll();
''', 'mdns refresh loop')

p.write_text(s)
print('PDL profile patch applied')
