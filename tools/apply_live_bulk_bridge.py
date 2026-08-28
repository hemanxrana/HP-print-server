from pathlib import Path

ROOT = Path('.')
PROBE = ROOT / 'experiments' / 'android_print_probe'


def replace_once(text, old, new, label):
    if old not in text:
        raise SystemExit(f'{label}: expected text not found')
    return text.replace(old, new, 1)

# usb_host_manager.h: add a read API for the selected classic printer interface.
p = PROBE / 'usb_host_manager.h'
s = p.read_text()
s = replace_once(s,
'''  bool bulkWrite(const uint8_t *data, size_t length, size_t &accepted,
                 uint32_t timeoutMs, String &error);
''',
'''  bool bulkWrite(const uint8_t *data, size_t length, size_t &accepted,
                 uint32_t timeoutMs, String &error);
  // Poll the selected classic Printer Class Bulk-IN endpoint. A normal USB
  // transfer timeout means "no backchannel bytes available yet" and returns
  // true with received == 0; hard USB errors return false.
  bool bulkRead(uint8_t *data, size_t capacity, size_t &received,
                uint32_t timeoutMs, String &error);
''', 'header bulkRead API')
p.write_text(s)

# usb_host_manager.cpp: do not consume another HCD channel by auto-claiming a
# protocol-0x04 interface. It remains selectable on demand from the dashboard.
p = PROBE / 'usb_host_manager.cpp'
s = p.read_text()
old_claim = '''  d.ippSelectedIndex = -1;
  uint8_t ippCandidate = 0;
  for (uint8_t i = 0; i < d.printerInterfaceCount; ++i) {
    const auto &ipp = d.printerInterfaces[i];
    if (!ipp.usableForIppUsb()) continue;
    const esp_err_t claim = usb_host_interface_claim(g.client, g.device,
                                                      ipp.interfaceNumber,
                                                      ipp.alternateSetting);
    if (claim == ESP_OK) {
      g.claimedIppInterface = ipp.interfaceNumber;
      g.ippInterfaceClaimed = true;
      d.ippSelectedIndex = (int8_t)ippCandidate;
      Serial.printf("[USB] Claimed IPP-over-USB candidate %u IF=%u ALT=%u\\n",
                    ippCandidate, ipp.interfaceNumber, ipp.alternateSetting);
      break;
    }
    Serial.printf("[USB] IPP-over-USB candidate %u IF=%u ALT=%u claim failed: %s\\n",
                  ippCandidate, ipp.interfaceNumber, ipp.alternateSetting,
                  esp_err_to_name(claim));
    ++ippCandidate;
  }
'''
new_claim = '''  // Keep protocol-0x04 interfaces unclaimed while classic IF1 printing is in
  // use. selectIppInterface() can still claim one explicitly later. This keeps
  // the live IF1 Bulk-IN/Bulk-OUT bridge from spending a scarce HCD channel on
  // an unrelated idle interface.
  d.ippSelectedIndex = -1;
'''
s = replace_once(s, old_claim, new_claim, 'remove automatic ipp claim')

# Insert classic Bulk-IN read implementation immediately before ippBulkWrite.
marker = '''bool UsbHostManager::ippBulkWrite(const uint8_t *data, size_t length, size_t &accepted,
'''
if marker not in s:
    raise SystemExit('cpp insert marker not found')
classic_read = r'''bool UsbHostManager::bulkRead(uint8_t *data, size_t capacity, size_t &received,
                              uint32_t timeoutMs, String &error) {
  received = 0;
  const UsbPrinterInterfaceInfo *iface = selectedInterface();
  if (!data || !capacity) { error = "empty classic Bulk-IN read buffer"; return false; }
  if (!g.deviceOpen || !g.printInterfaceClaimed || !iface || !iface->bulkIn.valid()) {
    error = "classic Printer Class Bulk-IN endpoint is not ready";
    return false;
  }
  if (capacity > 16384) { error = "classic Bulk-IN read exceeds 16 KiB"; return false; }

  usb_transfer_t *t = nullptr;
  esp_err_t e = usb_host_transfer_alloc(capacity, 0, &t);
  if (e != ESP_OK || !t) {
    error = String("classic Bulk-IN alloc failed: ") + esp_err_to_name(e);
    return false;
  }
  TransferWait w;
  w.done = xSemaphoreCreateBinary();
  if (!w.done) {
    usb_host_transfer_free(t);
    error = "unable to allocate classic Bulk-IN completion semaphore";
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
    error = String("classic Bulk-IN submit failed: ") + esp_err_to_name(e);
    return false;
  }

  const TickType_t waitTicks = timeoutMs ? pdMS_TO_TICKS(timeoutMs + 250) : portMAX_DELAY;
  if (xSemaphoreTake(w.done, waitTicks) != pdTRUE) {
    // This is an abnormal host/callback timeout, not the normal transfer timeout.
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

  // A short poll timing out is expected when the printer has no backchannel
  // data. Treat it as an empty successful poll so the TCP->USB direction never
  // stalls waiting for IN traffic.
  if (status == USB_TRANSFER_STATUS_TIMED_OUT ||
      (status == USB_TRANSFER_STATUS_COMPLETED && received == 0)) {
    received = 0;
    return true;
  }
  if (status != USB_TRANSFER_STATUS_COMPLETED) {
    error = String("classic Bulk-IN failed status=") + String((int)status) +
            " received=" + String((unsigned)received);
    return false;
  }
  return true;
}

'''
s = s.replace(marker, classic_read + marker, 1)
p.write_text(s)

# android_print_probe.ino: convert port 9100 from a capture sink into a live
# full-duplex byte-for-byte bridge to the selected IF1-style printer interface.
p = PROBE / 'android_print_probe.ino'
s = p.read_text()
s = replace_once(s,
'''static uint8_t ippResponseBuffer[IPP_RESPONSE_BUFFER_SIZE];
static uint8_t documentBuffer[DOCUMENT_BUFFER_SIZE];
''',
'''static uint8_t ippResponseBuffer[IPP_RESPONSE_BUFFER_SIZE];
static uint8_t documentBuffer[DOCUMENT_BUFFER_SIZE];
static uint8_t rawNetBuffer[4096];
static uint8_t rawUsbInBuffer[1024];

bool rawBridgeActive = false;
uint64_t rawBridgeLastNetToUsb = 0;
uint64_t rawBridgeLastUsbToNet = 0;
String rawBridgeLastError;
''', 'global bridge buffers')

old_raw = '''void serviceRaw() {
  WiFiClient client = rawServer.available();
  if (!client) return;
  Serial.printf("[PROBE][RAW] :9100 connection from %s:%u\\n", client.remoteIP().toString().c_str(), client.remotePort());
  uint8_t first[256] = {}; size_t firstLen = 0; uint64_t total = 0;
  uint32_t last = millis();
  while (client.connected() || client.available()) {
    while (client.available()) {
      uint8_t b = (uint8_t)client.read();
      if (firstLen < sizeof(first)) first[firstLen++] = b;
      ++total; last = millis();
    }
    if (millis() - last > 1000) break;
    delay(1);
  }
  Serial.printf("[PROBE][RAW] closed after %llu bytes; first=%u bytes captured\\n",
                (unsigned long long)total, (unsigned)firstLen);
  client.stop();
}
'''
new_raw = r'''void serviceRaw() {
  WiFiClient client = rawServer.available();
  if (!client) return;
  client.setNoDelay(true);

  rawBridgeActive = true;
  rawBridgeLastNetToUsb = 0;
  rawBridgeLastUsbToNet = 0;
  rawBridgeLastError = "";

  const UsbPrinterInterfaceInfo *raw = usbHost.selectedInterface();
  Serial.printf("[PROBE][RAW] LIVE duplex :9100 from %s:%u\n",
                client.remoteIP().toString().c_str(), client.remotePort());
  if (usbHost.state() != UsbHostManager::PRINTER_READY || !raw ||
      !raw->bulkOut.valid() || !raw->bulkIn.valid()) {
    rawBridgeLastError = "IF1-style classic printer Bulk-IN/Bulk-OUT interface is not ready";
    Serial.printf("[PROBE][RAW] bridge refused: %s\n", rawBridgeLastError.c_str());
    client.stop();
    rawBridgeActive = false;
    return;
  }

  Serial.printf("[PROBE][RAW] transparent bridge IF=%u ALT=%u proto=0x%02X OUT=0x%02X IN=0x%02X\n",
                raw->interfaceNumber, raw->alternateSetting, raw->protocol,
                raw->bulkOut.address, raw->bulkIn.address);

  uint32_t lastActivity = millis();
  uint32_t lastProgress = millis();
  bool bridgeOk = true;

  while ((client.connected() || client.available()) && bridgeOk) {
    // Wi-Fi -> USB Bulk OUT. Preserve bytes exactly; no parsing, PJL insertion,
    // PCL transformation, framing, or buffering of the full job.
    while (client.available() > 0 && bridgeOk) {
      const int want = min((int)sizeof(rawNetBuffer), client.available());
      const int got = client.read(rawNetBuffer, want);
      if (got <= 0) break;
      size_t offset = 0;
      while (offset < (size_t)got) {
        const size_t part = min((size_t)USB_CHUNK, (size_t)got - offset);
        size_t accepted = 0;
        String error;
        if (!usbHost.bulkWrite(rawNetBuffer + offset, part, accepted, 30000, error) || accepted != part) {
          rawBridgeLastError = error.length() ? error : "classic Bulk-OUT short write";
          bridgeOk = false;
          break;
        }
        offset += accepted;
        rawBridgeLastNetToUsb += accepted;
      }
      lastActivity = millis();
    }

    // USB Bulk IN -> same live TCP connection. Short 10 ms polls keep this
    // direction responsive without blocking the Wi-Fi -> OUT path.
    if (bridgeOk) {
      size_t received = 0;
      String error;
      if (!usbHost.bulkRead(rawUsbInBuffer, sizeof(rawUsbInBuffer), received, 10, error)) {
        rawBridgeLastError = error;
        bridgeOk = false;
      } else if (received) {
        size_t sent = 0;
        while (sent < received && client.connected()) {
          const size_t n = client.write(rawUsbInBuffer + sent, received - sent);
          if (!n) {
            rawBridgeLastError = "TCP write failed while forwarding printer Bulk-IN";
            bridgeOk = false;
            break;
          }
          sent += n;
        }
        rawBridgeLastUsbToNet += sent;
        lastActivity = millis();
      }
    }

    if (millis() - lastProgress >= 1000) {
      lastProgress = millis();
      Serial.printf("[PROBE][RAW] live net->usb=%llu usb->net=%llu\n",
                    (unsigned long long)rawBridgeLastNetToUsb,
                    (unsigned long long)rawBridgeLastUsbToNet);
    }

    // Keep a true session open while the client is connected. Five minutes of
    // total silence is only a safety escape for broken/stale sockets.
    if (millis() - lastActivity > 300000UL) {
      rawBridgeLastError = "RAW bridge idle timeout after 5 minutes";
      break;
    }
    delay(1);
  }

  // One final short IN drain lets immediate printer backchannel bytes reach a
  // still-connected client before close.
  if (client.connected()) {
    const uint32_t drainStart = millis();
    while (millis() - drainStart < 250) {
      size_t received = 0;
      String error;
      if (!usbHost.bulkRead(rawUsbInBuffer, sizeof(rawUsbInBuffer), received, 10, error)) break;
      if (received) {
        client.write(rawUsbInBuffer, received);
        rawBridgeLastUsbToNet += received;
      }
      delay(1);
    }
  }

  Serial.printf("[PROBE][RAW] LIVE closed net->usb=%llu usb->net=%llu ok=%d%s%s\n",
                (unsigned long long)rawBridgeLastNetToUsb,
                (unsigned long long)rawBridgeLastUsbToNet,
                bridgeOk ? 1 : 0,
                rawBridgeLastError.length() ? " error=" : "",
                rawBridgeLastError.c_str());
  client.stop();
  rawBridgeActive = false;
}
'''
s = replace_once(s, old_raw, new_raw, 'replace capture-only raw service')

# Add bridge telemetry to dashboard before memory/stability.
needle = '''  html += "<section><h2>6. Memory / stability</h2><table>";
'''
replacement = '''  html += "<section><h2>6. Live RAW 9100 ↔ USB IF1 bridge</h2><table>";
  html += "<tr><th>Status</th><td>" + String(rawBridgeActive ? "ACTIVE" : "idle") + "</td></tr>";
  html += "<tr><th>Wi-Fi → USB OUT</th><td>" + String((unsigned long long)rawBridgeLastNetToUsb) + " bytes</td></tr>";
  html += "<tr><th>USB IN → Wi-Fi</th><td>" + String((unsigned long long)rawBridgeLastUsbToNet) + " bytes</td></tr>";
  if (rawBridgeLastError.length()) html += "<tr><th>Last error</th><td>" + htmlEscape(rawBridgeLastError) + "</td></tr>";
  html += "</table><p>TCP 9100 is byte-transparent and full-duplex: network bytes go directly to the selected classic Printer Class Bulk-OUT endpoint, and printer Bulk-IN bytes are returned live on the same TCP connection. No PDL conversion is performed.</p></section>";

  html += "<section><h2>7. Memory / stability</h2><table>";
'''
s = replace_once(s, needle, replacement, 'dashboard bridge telemetry')

# Update headline/instructions so port 9100 behavior is unambiguous.
s = s.replace('Android Print Probe — one flash', 'Android Print Probe — live duplex bridge', 2)
s = s.replace('Safe Capture is the default after every reboot',
              'Safe Capture is the default after every reboot; TCP 9100 is always a transparent IF1 duplex bridge', 1)
s = s.replace('rawServer.begin(); ippServer.setNoDelay(true); rawServer.setNoDelay(true);',
              'rawServer.begin(); ippServer.setNoDelay(true); rawServer.setNoDelay(true);', 1)
p.write_text(s)

print('Live IF1 transparent duplex bridge patch applied')
