from pathlib import Path

ROOT = Path('.')
PROBE = ROOT / 'experiments' / 'android_print_probe'


def replace_once(text, old, new, label):
    if old not in text:
        raise SystemExit(f'{label}: expected text not found')
    return text.replace(old, new, 1)

# Add a non-erroring short-poll API for the selected IPP-over-USB Bulk-IN endpoint.
p = PROBE / 'usb_host_manager.h'
s = p.read_text()
s = replace_once(s,
'''  bool ippBulkRead(uint8_t *data, size_t capacity, size_t &received,
                   uint32_t timeoutMs, String &error);
''',
'''  bool ippBulkRead(uint8_t *data, size_t capacity, size_t &received,
                   uint32_t timeoutMs, String &error);
  // Interactive IPP bridge poll: a normal transfer timeout means that the
  // printer has no response bytes right now and returns true with received=0.
  bool ippBulkReadPoll(uint8_t *data, size_t capacity, size_t &received,
                       uint32_t timeoutMs, String &error);
''', 'header ipp poll API')
p.write_text(s)

# Implement the short-poll reader beside the existing blocking IPP reader.
p = PROBE / 'usb_host_manager.cpp'
s = p.read_text()
marker = '''void UsbHostManager::onPortStatusTransfer(bool valid, uint8_t value, const String &error) {
'''
if marker not in s:
    raise SystemExit('cpp poll insert marker not found')
impl = r'''bool UsbHostManager::ippBulkReadPoll(uint8_t *data, size_t capacity, size_t &received,
                                     uint32_t timeoutMs, String &error) {
  received = 0;
  const int8_t selected = device_.ippSelectedIndex;
  const UsbPrinterInterfaceInfo *iface = selected >= 0 ? ippInterfaceAt((uint8_t)selected) : nullptr;
  if (!data || !capacity) { error = "empty IPP-over-USB poll buffer"; return false; }
  if (!g.deviceOpen || !g.ippInterfaceClaimed || !iface || !iface->usableForIppUsb()) {
    error = "IPP-over-USB interface is not claimed/ready";
    return false;
  }
  if (capacity > 16384) capacity = 16384;

  usb_transfer_t *t = nullptr;
  esp_err_t e = usb_host_transfer_alloc(capacity, 0, &t);
  if (e != ESP_OK || !t) {
    error = String("IPP-over-USB poll alloc failed: ") + esp_err_to_name(e);
    return false;
  }
  TransferWait w;
  w.done = xSemaphoreCreateBinary();
  if (!w.done) {
    usb_host_transfer_free(t);
    error = "unable to allocate IPP-over-USB poll semaphore";
    return false;
  }

  t->num_bytes = capacity;
  t->device_handle = g.device;
  t->bEndpointAddress = iface->bulkIn.address;
  t->callback = bulkTransferCallback;
  t->context = &w;
  t->timeout_ms = timeoutMs;

  e = usb_host_transfer_submit(t);
  if (e != ESP_OK) {
    vSemaphoreDelete(w.done);
    usb_host_transfer_free(t);
    error = String("IPP-over-USB poll submit failed: ") + esp_err_to_name(e);
    return false;
  }

  const TickType_t waitTicks = timeoutMs ? pdMS_TO_TICKS(timeoutMs + 250) : portMAX_DELAY;
  if (xSemaphoreTake(w.done, waitTicks) != pdTRUE) {
    usb_host_endpoint_halt(t->device_handle, t->bEndpointAddress);
    usb_host_endpoint_flush(t->device_handle, t->bEndpointAddress);
    usb_host_endpoint_clear(t->device_handle, t->bEndpointAddress);
    xSemaphoreTake(w.done, portMAX_DELAY);
  }

  received = static_cast<size_t>(t->actual_num_bytes);
  const usb_transfer_status_t status = t->status;
  if (status == USB_TRANSFER_STATUS_COMPLETED && received) {
    memcpy(data, t->data_buffer, received);
  }
  vSemaphoreDelete(w.done);
  usb_host_transfer_free(t);

  if (status == USB_TRANSFER_STATUS_TIMED_OUT ||
      (status == USB_TRANSFER_STATUS_COMPLETED && received == 0)) {
    received = 0;
    return true;
  }
  if (status != USB_TRANSFER_STATUS_COMPLETED) {
    error = String("IPP-over-USB poll failed status=") + String((int)status) +
            " received=" + String((unsigned)received);
    return false;
  }
  return true;
}

'''
s = s.replace(marker, impl + marker, 1)
p.write_text(s)

# Add a fourth test mode and live bridge telemetry/buffers.
p = PROBE / 'android_print_probe.ino'
s = p.read_text()
s = replace_once(s,
'''static uint8_t rawNetBuffer[4096];
static uint8_t rawUsbInBuffer[1024];

bool rawBridgeActive = false;
''',
'''static uint8_t rawNetBuffer[4096];
static uint8_t rawUsbInBuffer[1024];
static uint8_t ippLiveNetBuffer[1024];
static uint8_t ippLiveUsbInBuffer[1024];

bool ippLiveActive = false;
uint64_t ippLiveNetToUsb = 0;
uint64_t ippLiveUsbToNet = 0;
uint32_t ippLiveOutTransfers = 0;
uint32_t ippLiveInTransfers = 0;
String ippLiveLastResponse;
String ippLiveLastError;

bool rawBridgeActive = false;
''', 'live ipp globals')

s = replace_once(s,
'''enum ProbeMode : uint8_t { MODE_SAFE = 0, MODE_CLASSIC_RAW = 1, MODE_IPP_USB = 2 };
''',
'''enum ProbeMode : uint8_t {
  MODE_SAFE = 0,
  MODE_CLASSIC_RAW = 1,
  MODE_IPP_USB = 2,
  MODE_IPP_LIVE = 3
};
''', 'add live mode enum')

s = replace_once(s,
'''    case MODE_IPP_USB: return "IPP-OVER-USB EXPERIMENTAL";
''',
'''    case MODE_IPP_USB: return "IPP-OVER-USB REBUILT";
    case MODE_IPP_LIVE: return "LIVE IPP USB DUPLEX";
''', 'mode name')

# Insert the live HTTP-over-USB proxy before handleIppClient(). It forwards the
# network request body as raw HTTP bytes (including chunk framing) and polls
# USB Bulk-IN between every small OUT chunk so 100-Continue/status/final replies
# can flow back while the request is still in progress.
marker = '''void handleIppClient(WiFiClient client) {
'''
if marker not in s:
    raise SystemExit('ino live bridge insert marker not found')
live_code = r'''String normalizeLiveIppHeader(const String &header) {
  String out;
  out.reserve(header.length() + 64);
  int pos = 0;
  bool hostDone = false;
  bool connectionDone = false;
  while (pos < header.length()) {
    int end = header.indexOf("\r\n", pos);
    if (end < 0) break;
    String line = header.substring(pos, end);
    pos = end + 2;
    if (line.length() == 0) break;
    String lower = line;
    lower.toLowerCase();
    if (lower.startsWith("host:")) {
      out += "Host: localhost\r\n";
      hostDone = true;
    } else if (lower.startsWith("connection:")) {
      out += "Connection: close\r\n";
      connectionDone = true;
    } else {
      out += line;
      out += "\r\n";
    }
  }
  if (!hostDone) out += "Host: localhost\r\n";
  if (!connectionDone) out += "Connection: close\r\n";
  out += "\r\n";
  return out;
}

bool liveIppUsbWrite(const uint8_t *data, size_t length, String &error) {
  size_t offset = 0;
  while (offset < length) {
    const size_t part = min((size_t)USB_CHUNK, length - offset);
    size_t accepted = 0;
    if (!usbHost.ippBulkWrite(data + offset, part, accepted, 30000, error)) return false;
    if (accepted != part) {
      error = "live IPP USB short OUT write";
      return false;
    }
    offset += accepted;
    ippLiveNetToUsb += accepted;
    ++ippLiveOutTransfers;
  }
  return true;
}

void noteLiveIppResponseBytes(const uint8_t *data, size_t length) {
  if (ippLiveLastResponse.length() >= 512) return;
  for (size_t i = 0; i < length && ippLiveLastResponse.length() < 512; ++i) {
    const char c = (char)data[i];
    if (c == '\r') continue;
    if (c == '\n') {
      if (ippLiveLastResponse.length()) break;
      continue;
    }
    if ((uint8_t)c >= 32 && (uint8_t)c <= 126) ippLiveLastResponse += c;
    else ippLiveLastResponse += '.';
  }
}

bool pumpLiveIppUsbIn(WiFiClient &client, bool &gotBytes, bool &finalResponseSeen,
                      uint32_t pollMs, String &error) {
  gotBytes = false;
  size_t received = 0;
  if (!usbHost.ippBulkReadPoll(ippLiveUsbInBuffer, sizeof(ippLiveUsbInBuffer), received,
                               pollMs, error)) return false;
  if (!received) return true;
  gotBytes = true;
  ++ippLiveInTransfers;
  ippLiveUsbToNet += received;
  noteLiveIppResponseBytes(ippLiveUsbInBuffer, received);

  size_t sent = 0;
  while (sent < received && client.connected()) {
    const size_t n = client.write(ippLiveUsbInBuffer + sent, received - sent);
    if (!n) {
      error = "TCP write failed while returning live IPP USB Bulk-IN";
      return false;
    }
    sent += n;
  }
  client.flush();

  // We only inspect a tiny response prefix for lifecycle control; bytes are
  // already forwarded unchanged to Android. 100 Continue is interim, while a
  // later HTTP status line is treated as the final response.
  static String responseProbe;
  if (responseProbe.length() < 2048) {
    for (size_t i = 0; i < received && responseProbe.length() < 2048; ++i) {
      responseProbe += (char)ippLiveUsbInBuffer[i];
    }
    int first = responseProbe.indexOf("HTTP/");
    while (first >= 0) {
      int eol = responseProbe.indexOf("\r\n", first);
      if (eol < 0) break;
      String line = responseProbe.substring(first, eol);
      const bool interim100 = line.indexOf(" 100 ") >= 0 || line.endsWith(" 100");
      if (!interim100) finalResponseSeen = true;
      first = responseProbe.indexOf("HTTP/", eol + 2);
    }
  }
  if (finalResponseSeen) responseProbe = "";
  return true;
}

void handleLiveIppUsb(WiFiClient &client, const String &networkHeader) {
  ippLiveActive = true;
  ippLiveNetToUsb = 0;
  ippLiveUsbToNet = 0;
  ippLiveOutTransfers = 0;
  ippLiveInTransfers = 0;
  ippLiveLastResponse = "";
  ippLiveLastError = "";

  const int8_t selected = usbHost.selectedIppInterfaceIndex();
  const UsbPrinterInterfaceInfo *iface = selected >= 0 ? usbHost.ippInterfaceAt((uint8_t)selected) : nullptr;
  if (!iface) {
    ippLiveLastError = "No explicitly selected IPP-over-USB interface";
    client.print("HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
    client.stop();
    ippLiveActive = false;
    return;
  }

  Serial.printf("[PROBE][IPP-LIVE] start candidate=%d IF=%u ALT=%u OUT=0x%02X IN=0x%02X\n",
                (int)selected, iface->interfaceNumber, iface->alternateSetting,
                iface->bulkOut.address, iface->bulkIn.address);

  const String usbHeader = normalizeLiveIppHeader(networkHeader);
  String error;
  if (!liveIppUsbWrite((const uint8_t *)usbHeader.c_str(), usbHeader.length(), error)) {
    ippLiveLastError = error;
    client.stop();
    ippLiveActive = false;
    return;
  }

  bool ok = true;
  bool finalResponseSeen = false;
  uint32_t lastAnyActivity = millis();
  uint32_t lastUsbIn = 0;
  uint32_t lastProgress = millis();

  while (ok && (client.connected() || client.available())) {
    bool didWork = false;

    // Forward at most one small TCP chunk per turn, then give Bulk-IN a chance.
    // This prevents a multi-megabyte Print-Job from starving printer replies.
    if (client.available() > 0) {
      const int want = min((int)sizeof(ippLiveNetBuffer), client.available());
      const int got = client.read(ippLiveNetBuffer, want);
      if (got > 0) {
        if (!liveIppUsbWrite(ippLiveNetBuffer, (size_t)got, error)) {
          ok = false;
          ippLiveLastError = error;
        } else {
          didWork = true;
          lastAnyActivity = millis();
        }
      }
    }

    if (ok) {
      bool gotUsb = false;
      if (!pumpLiveIppUsbIn(client, gotUsb, finalResponseSeen, 5, error)) {
        ok = false;
        ippLiveLastError = error;
      } else if (gotUsb) {
        didWork = true;
        lastAnyActivity = millis();
        lastUsbIn = millis();
      }
    }

    if (millis() - lastProgress >= 1000) {
      lastProgress = millis();
      Serial.printf("[PROBE][IPP-LIVE] net->usb=%llu usb->net=%llu OUT=%lu IN=%lu final=%d\n",
                    (unsigned long long)ippLiveNetToUsb,
                    (unsigned long long)ippLiveUsbToNet,
                    (unsigned long)ippLiveOutTransfers,
                    (unsigned long)ippLiveInTransfers,
                    finalResponseSeen ? 1 : 0);
    }

    // Once a non-100 HTTP response has arrived and Bulk-IN has gone quiet,
    // this request/response transaction is complete. Keep a short drain window
    // so the tail of the IPP response is not truncated.
    if (finalResponseSeen && lastUsbIn && millis() - lastUsbIn >= 300 && client.available() == 0) break;

    // Pure bridge mode intentionally waits for the real USB side. This catches
    // missing 100-Continue or a silent protocol-0x04 endpoint instead of faking
    // a local IPP response.
    if (!didWork && millis() - lastAnyActivity >= 30000) {
      ippLiveLastError = "live IPP bridge idle: no TCP body or USB response for 30 seconds";
      ok = false;
      break;
    }
    delay(1);
  }

  // Final short Bulk-IN drain while Android is still connected.
  if (client.connected()) {
    const uint32_t drainStart = millis();
    while (millis() - drainStart < 250) {
      bool gotUsb = false;
      String drainError;
      if (!pumpLiveIppUsbIn(client, gotUsb, finalResponseSeen, 5, drainError)) break;
      if (!gotUsb) delay(1);
    }
  }

  Serial.printf("[PROBE][IPP-LIVE] closed net->usb=%llu usb->net=%llu OUT=%lu IN=%lu ok=%d response='%s'%s%s\n",
                (unsigned long long)ippLiveNetToUsb,
                (unsigned long long)ippLiveUsbToNet,
                (unsigned long)ippLiveOutTransfers,
                (unsigned long)ippLiveInTransfers,
                ok ? 1 : 0, ippLiveLastResponse.c_str(),
                ippLiveLastError.length() ? " error=" : "",
                ippLiveLastError.c_str());
  client.stop();
  ippLiveActive = false;
}

'''
s = s.replace(marker, live_code + marker, 1)

# Divert the entire HTTP request to the selected protocol-0x04 interface before
# the local IPP parser consumes any body bytes. This keeps chunk framing and the
# original IPP body byte-for-byte intact.
needle = '''  Serial.printf("[PROBE][IPP] %s transfer=%s content-length=%s\\n",
                firstLine.c_str(), chunked ? "chunked" : "fixed", lengthText.c_str());

  if (expect.equalsIgnoreCase("100-continue")) {
'''
replacement = '''  Serial.printf("[PROBE][IPP] %s transfer=%s content-length=%s\\n",
                firstLine.c_str(), chunked ? "chunked" : "fixed", lengthText.c_str());

  if (probeMode == MODE_IPP_LIVE) {
    handleLiveIppUsb(client, header);
    return;
  }

  if (expect.equalsIgnoreCase("100-continue")) {
'''
s = replace_once(s, needle, replacement, 'route live mode before parser')

# Dashboard mode control.
s = replace_once(s,
'''  else if (mode == "ippusb") {
    if (usbHost.selectedIppInterfaceIndex() < 0) {
      web.send(409, "text/plain", "No IPP-over-USB interface is currently claimed"); return;
    }
    probeMode = MODE_IPP_USB;
  } else { web.send(400, "text/plain", "Invalid mode"); return; }
''',
'''  else if (mode == "ippusb" || mode == "ipplive") {
    if (usbHost.selectedIppInterfaceIndex() < 0) {
      web.send(409, "text/plain", "Select and claim a protocol-0x04 IPP-over-USB interface first"); return;
    }
    probeMode = mode == "ipplive" ? MODE_IPP_LIVE : MODE_IPP_USB;
  } else { web.send(400, "text/plain", "Invalid mode"); return; }
''', 'mode handler live option')

s = replace_once(s,
'''  html += "<label><input type='radio' name='mode' value='ippusb' " + String(probeMode == MODE_IPP_USB ? "checked" : "") + "> IPP-over-USB experimental — rebuild HTTP/IPP and proxy through protocol 0x04</label><button>Arm selected mode</button></form><p class='warn'>Mode resets to Safe Capture after reboot.</p></section>";
''',
'''  html += "<label><input type='radio' name='mode' value='ippusb' " + String(probeMode == MODE_IPP_USB ? "checked" : "") + "> IPP-over-USB rebuilt — parse Android IPP, rebuild HTTP/IPP, then proxy through protocol 0x04</label>";
  html += "<label><input type='radio' name='mode' value='ipplive' " + String(probeMode == MODE_IPP_LIVE ? "checked" : "") + "> Live IPP USB duplex — pass HTTP/IPP body/chunk bytes directly to USB OUT and return USB IN live on the same TCP connection</label><button>Arm selected mode</button></form><p class='warn'>Live mode requires an explicitly selected protocol-0x04 candidate. It does not fake 100-Continue: the real USB endpoint must answer. Mode resets to Safe Capture after reboot.</p></section>";
''', 'dashboard live mode radio')

# Add live IPP telemetry before RAW telemetry.
needle = '''  html += "<section><h2>6. Live RAW 9100 ↔ USB IF1 bridge</h2><table>";
'''
replacement = '''  html += "<section><h2>6. Live IPP 631 ↔ protocol-0x04 USB duplex</h2><table>";
  html += "<tr><th>Status</th><td>" + String(ippLiveActive ? "ACTIVE" : "idle") + "</td></tr>";
  html += "<tr><th>Network → USB OUT</th><td>" + String((unsigned long long)ippLiveNetToUsb) + " bytes</td></tr>";
  html += "<tr><th>USB IN → Network</th><td>" + String((unsigned long long)ippLiveUsbToNet) + " bytes</td></tr>";
  html += "<tr><th>OUT transfers</th><td>" + String((unsigned long)ippLiveOutTransfers) + "</td></tr>";
  html += "<tr><th>IN transfers with data</th><td>" + String((unsigned long)ippLiveInTransfers) + "</td></tr>";
  if (ippLiveLastResponse.length()) html += "<tr><th>Last USB HTTP response</th><td><code>" + htmlEscape(ippLiveLastResponse) + "</code></td></tr>";
  if (ippLiveLastError.length()) html += "<tr><th>Last error</th><td>" + htmlEscape(ippLiveLastError) + "</td></tr>";
  html += "</table><p>In Live IPP USB Duplex mode the ESP32 only normalizes Host/Connection headers. The IPP body and HTTP chunk bytes are forwarded unchanged, and protocol-0x04 Bulk-IN is polled between every small OUT transfer so 100-Continue, status and final responses can reach Android during the request.</p></section>";

  html += "<section><h2>7. Live RAW 9100 ↔ USB IF1 bridge</h2><table>";
'''
s = replace_once(s, needle, replacement, 'dashboard live ipp telemetry')

s = s.replace('<section><h2>7. Memory / stability</h2>', '<section><h2>8. Memory / stability</h2>', 1)
s = s.replace('5) If no output, select IPP-over-USB and try candidate 0, then candidates 1/2 only if needed.',
              '5) For protocol-0x04 testing, explicitly select a candidate, then choose Rebuilt IPP or Live IPP USB Duplex. Live mode forwards the real printer HTTP response instead of synthesizing one.', 1)

p.write_text(s)
print('Live IPP USB duplex proxy patch applied')
