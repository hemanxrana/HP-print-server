from pathlib import Path

ROOT = Path('.')
PROBE = ROOT / 'experiments' / 'android_print_probe'


def replace_once(text, old, new, label):
    if old not in text:
        raise SystemExit(f'{label}: expected text not found')
    return text.replace(old, new, 1)

# Public API: one live session owns a reusable OUT transfer and a continuously
# armed IN transfer whose callback queues printer bytes for the network loop.
p = PROBE / 'usb_host_manager.h'
s = p.read_text()
s = replace_once(s,
'''  bool ippBulkReadPoll(uint8_t *data, size_t capacity, size_t &received,
                       uint32_t timeoutMs, String &error);
''',
'''  bool ippBulkReadPoll(uint8_t *data, size_t capacity, size_t &received,
                       uint32_t timeoutMs, String &error);

  // Persistent full-duplex IPP-over-USB session. Bulk-IN remains armed while
  // Bulk-OUT writes are in flight, so printer responses can arrive truly
  // concurrently instead of being polled only between writes.
  bool beginIppLiveIo(String &error);
  void endIppLiveIo();
  bool ippLiveWrite(const uint8_t *data, size_t length, size_t &accepted,
                    uint32_t timeoutMs, String &error);
  bool ippLiveReadAvailable(uint8_t *data, size_t capacity, size_t &received,
                            String &error);
''', 'header persistent live API')
p.write_text(s)

p = PROBE / 'usb_host_manager.cpp'
s = p.read_text()
s = replace_once(s,
'''#include "freertos/semphr.h"
#include "freertos/task.h"
''',
'''#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
''', 'stream buffer include')

s = replace_once(s,
'''static constexpr uint32_t STATUS_TRANSFER_TIMEOUT_MS = 1000;
''',
'''static constexpr uint32_t STATUS_TRANSFER_TIMEOUT_MS = 1000;
static constexpr size_t IPP_LIVE_TRANSFER_BYTES = 1024;
static constexpr size_t IPP_LIVE_RX_STREAM_BYTES = 8192;
static constexpr uint32_t IPP_LIVE_IN_TIMEOUT_MS = 50;
''', 'live constants')

s = replace_once(s,
'''  volatile usb_transfer_t *statusTransfer = nullptr;

  TaskHandle_t clientTask = nullptr;
''',
'''  volatile usb_transfer_t *statusTransfer = nullptr;

  usb_transfer_t *ippLiveInTransfer = nullptr;
  usb_transfer_t *ippLiveOutTransfer = nullptr;
  SemaphoreHandle_t ippLiveOutDone = nullptr;
  SemaphoreHandle_t ippLiveInStopped = nullptr;
  StreamBufferHandle_t ippLiveRx = nullptr;
  volatile bool ippLiveRunning = false;
  volatile bool ippLiveInSubmitted = false;
  volatile int ippLiveErrorStatus = 0;

  TaskHandle_t clientTask = nullptr;
''', 'runtime persistent state')

# Stop live transfers before an interface/device can be released.
s = replace_once(s,
'''static void releaseInterfaces() {
  if (!g.deviceOpen || !g.device) return;
''',
'''static void releaseInterfaces() {
  if (manager) manager->endIppLiveIo();
  if (!g.deviceOpen || !g.device) return;
''', 'stop live on release')

# Add callbacks next to the ordinary transfer callback.
marker = '''static void statusTransferCallback(usb_transfer_t *t) {
'''
if marker not in s:
    raise SystemExit('callback insert marker not found')
callbacks = r'''static void ippLiveOutCallback(usb_transfer_t *t) {
  if (!t) return;
  if (g.ippLiveOutDone) xSemaphoreGive(g.ippLiveOutDone);
}

static void ippLiveInCallback(usb_transfer_t *t) {
  if (!t) return;
  g.ippLiveInSubmitted = false;

  if (t->status == USB_TRANSFER_STATUS_COMPLETED && t->actual_num_bytes && g.ippLiveRx) {
    const size_t wanted = static_cast<size_t>(t->actual_num_bytes);
    const size_t queued = xStreamBufferSend(g.ippLiveRx, t->data_buffer, wanted, 0);
    if (queued != wanted) {
      g.ippLiveErrorStatus = -10001;  // RX stream overflow
      g.ippLiveRunning = false;
    }
  } else if (t->status != USB_TRANSFER_STATUS_COMPLETED &&
             t->status != USB_TRANSFER_STATUS_TIMED_OUT) {
    g.ippLiveErrorStatus = 1000 + (int)t->status;
    g.ippLiveRunning = false;
  }

  if (g.ippLiveRunning) {
    t->num_bytes = IPP_LIVE_TRANSFER_BYTES;
    t->timeout_ms = IPP_LIVE_IN_TIMEOUT_MS;
    const esp_err_t e = usb_host_transfer_submit(t);
    if (e == ESP_OK) {
      g.ippLiveInSubmitted = true;
      return;
    }
    g.ippLiveErrorStatus = -(int)e;
    g.ippLiveRunning = false;
  }

  if (g.ippLiveInStopped) xSemaphoreGive(g.ippLiveInStopped);
}

'''
s = s.replace(marker, callbacks + marker, 1)

# Ensure selecting a different protocol-0x04 interface first ends any live session.
s = replace_once(s,
'''  if (device_.ippSelectedIndex == (int8_t)index && g.ippInterfaceClaimed) return true;

  if (g.ippInterfaceClaimed) {
''',
'''  if (device_.ippSelectedIndex == (int8_t)index && g.ippInterfaceClaimed) return true;

  endIppLiveIo();
  if (g.ippInterfaceClaimed) {
''', 'stop live before interface switch')

# Insert persistent session implementation before the existing one-shot poll.
marker = '''bool UsbHostManager::ippBulkReadPoll(uint8_t *data, size_t capacity, size_t &received,
'''
if marker not in s:
    raise SystemExit('persistent implementation marker not found')
impl = r'''bool UsbHostManager::beginIppLiveIo(String &error) {
  if (g.ippLiveRunning && g.ippLiveInTransfer && g.ippLiveOutTransfer && g.ippLiveRx) return true;
  endIppLiveIo();

  const int8_t selected = device_.ippSelectedIndex;
  const UsbPrinterInterfaceInfo *iface = selected >= 0 ? ippInterfaceAt((uint8_t)selected) : nullptr;
  if (!g.deviceOpen || !g.ippInterfaceClaimed || !iface || !iface->usableForIppUsb()) {
    error = "IPP live session requires a claimed protocol-0x04 interface";
    return false;
  }

  g.ippLiveOutDone = xSemaphoreCreateBinary();
  g.ippLiveInStopped = xSemaphoreCreateBinary();
  g.ippLiveRx = xStreamBufferCreate(IPP_LIVE_RX_STREAM_BYTES, 1);
  if (!g.ippLiveOutDone || !g.ippLiveInStopped || !g.ippLiveRx) {
    error = "unable to allocate IPP live synchronization buffers";
    endIppLiveIo();
    return false;
  }

  esp_err_t e = usb_host_transfer_alloc(IPP_LIVE_TRANSFER_BYTES, 0, &g.ippLiveOutTransfer);
  if (e != ESP_OK || !g.ippLiveOutTransfer) {
    error = String("IPP live OUT transfer alloc failed: ") + esp_err_to_name(e);
    endIppLiveIo();
    return false;
  }
  e = usb_host_transfer_alloc(IPP_LIVE_TRANSFER_BYTES, 0, &g.ippLiveInTransfer);
  if (e != ESP_OK || !g.ippLiveInTransfer) {
    error = String("IPP live IN transfer alloc failed: ") + esp_err_to_name(e);
    endIppLiveIo();
    return false;
  }

  g.ippLiveOutTransfer->device_handle = g.device;
  g.ippLiveOutTransfer->bEndpointAddress = iface->bulkOut.address;
  g.ippLiveOutTransfer->callback = ippLiveOutCallback;
  g.ippLiveOutTransfer->context = nullptr;

  g.ippLiveInTransfer->num_bytes = IPP_LIVE_TRANSFER_BYTES;
  g.ippLiveInTransfer->device_handle = g.device;
  g.ippLiveInTransfer->bEndpointAddress = iface->bulkIn.address;
  g.ippLiveInTransfer->callback = ippLiveInCallback;
  g.ippLiveInTransfer->context = nullptr;
  g.ippLiveInTransfer->timeout_ms = IPP_LIVE_IN_TIMEOUT_MS;

  g.ippLiveErrorStatus = 0;
  g.ippLiveRunning = true;
  e = usb_host_transfer_submit(g.ippLiveInTransfer);
  if (e != ESP_OK) {
    g.ippLiveRunning = false;
    error = String("IPP live IN initial submit failed: ") + esp_err_to_name(e);
    endIppLiveIo();
    return false;
  }
  g.ippLiveInSubmitted = true;
  Serial.printf("[USB][IPP-LIVE] persistent IN armed EP=0x%02X; reusable OUT EP=0x%02X\n",
                iface->bulkIn.address, iface->bulkOut.address);
  return true;
}

void UsbHostManager::endIppLiveIo() {
  const bool hadResources = g.ippLiveInTransfer || g.ippLiveOutTransfer ||
                            g.ippLiveOutDone || g.ippLiveInStopped || g.ippLiveRx;
  g.ippLiveRunning = false;

  if (g.ippLiveInSubmitted && g.ippLiveInStopped) {
    if (xSemaphoreTake(g.ippLiveInStopped, pdMS_TO_TICKS(150)) != pdTRUE &&
        g.deviceOpen && g.device && g.ippLiveInTransfer) {
      usb_host_endpoint_halt(g.device, g.ippLiveInTransfer->bEndpointAddress);
      usb_host_endpoint_flush(g.device, g.ippLiveInTransfer->bEndpointAddress);
      usb_host_endpoint_clear(g.device, g.ippLiveInTransfer->bEndpointAddress);
      xSemaphoreTake(g.ippLiveInStopped, pdMS_TO_TICKS(250));
    }
  }
  g.ippLiveInSubmitted = false;

  if (g.ippLiveInTransfer) {
    usb_host_transfer_free(g.ippLiveInTransfer);
    g.ippLiveInTransfer = nullptr;
  }
  if (g.ippLiveOutTransfer) {
    usb_host_transfer_free(g.ippLiveOutTransfer);
    g.ippLiveOutTransfer = nullptr;
  }
  if (g.ippLiveRx) {
    vStreamBufferDelete(g.ippLiveRx);
    g.ippLiveRx = nullptr;
  }
  if (g.ippLiveOutDone) {
    vSemaphoreDelete(g.ippLiveOutDone);
    g.ippLiveOutDone = nullptr;
  }
  if (g.ippLiveInStopped) {
    vSemaphoreDelete(g.ippLiveInStopped);
    g.ippLiveInStopped = nullptr;
  }
  g.ippLiveErrorStatus = 0;
  if (hadResources) Serial.println("[USB][IPP-LIVE] persistent session stopped");
}

bool UsbHostManager::ippLiveWrite(const uint8_t *data, size_t length, size_t &accepted,
                                  uint32_t timeoutMs, String &error) {
  accepted = 0;
  if (!data || !length) { error = "empty IPP live OUT transfer"; return false; }
  if (!g.ippLiveRunning || !g.ippLiveOutTransfer || !g.ippLiveOutDone) {
    error = "IPP live session is not running";
    return false;
  }
  if (length > IPP_LIVE_TRANSFER_BYTES) {
    error = "IPP live OUT chunk exceeds persistent transfer capacity";
    return false;
  }

  usb_transfer_t *t = g.ippLiveOutTransfer;
  memcpy(t->data_buffer, data, length);
  t->num_bytes = length;
  t->timeout_ms = timeoutMs;
  xSemaphoreTake(g.ippLiveOutDone, 0);

  const esp_err_t e = usb_host_transfer_submit(t);
  if (e != ESP_OK) {
    error = String("IPP live OUT submit failed: ") + esp_err_to_name(e);
    return false;
  }
  const TickType_t ticks = timeoutMs ? pdMS_TO_TICKS(timeoutMs + 250) : portMAX_DELAY;
  if (xSemaphoreTake(g.ippLiveOutDone, ticks) != pdTRUE) {
    error = "IPP live OUT completion timeout";
    return false;
  }

  accepted = static_cast<size_t>(t->actual_num_bytes);
  if (t->status != USB_TRANSFER_STATUS_COMPLETED || accepted != length) {
    error = String("IPP live OUT failed status=") + String((int)t->status) +
            " accepted=" + String((unsigned)accepted) + "/" + String((unsigned)length);
    return false;
  }
  return true;
}

bool UsbHostManager::ippLiveReadAvailable(uint8_t *data, size_t capacity, size_t &received,
                                          String &error) {
  received = 0;
  if (!data || !capacity) { error = "empty IPP live IN read buffer"; return false; }
  if (!g.ippLiveRx) { error = "IPP live RX stream is not allocated"; return false; }

  received = xStreamBufferReceive(g.ippLiveRx, data, capacity, 0);
  if (received) return true;
  if (!g.ippLiveRunning && g.ippLiveErrorStatus) {
    if (g.ippLiveErrorStatus == -10001) error = "IPP live IN queue overflow";
    else error = String("IPP live IN stopped status=") + String(g.ippLiveErrorStatus);
    return false;
  }
  return true;
}

'''
s = s.replace(marker, impl + marker, 1)
p.write_text(s)

# Switch the live proxy to the persistent session APIs. This keeps IN armed
# during every synchronous OUT wait, which is the key full-duplex improvement.
p = PROBE / 'android_print_probe.ino'
s = p.read_text()
s = replace_once(s,
'''    if (!usbHost.ippBulkWrite(data + offset, part, accepted, 30000, error)) return false;
''',
'''    if (!usbHost.ippLiveWrite(data + offset, part, accepted, 30000, error)) return false;
''', 'live writer persistent API')

s = replace_once(s,
'''  if (!usbHost.ippBulkReadPoll(ippLiveUsbInBuffer, sizeof(ippLiveUsbInBuffer), received,
                               pollMs, error)) return false;
''',
'''  (void)pollMs;
  if (!usbHost.ippLiveReadAvailable(ippLiveUsbInBuffer, sizeof(ippLiveUsbInBuffer),
                                    received, error)) return false;
''', 'live reader persistent queue')

needle = '''  Serial.printf("[PROBE][IPP-LIVE] start candidate=%d IF=%u ALT=%u OUT=0x%02X IN=0x%02X\\n",
                (int)selected, iface->interfaceNumber, iface->alternateSetting,
                iface->bulkOut.address, iface->bulkIn.address);

  const String usbHeader = normalizeLiveIppHeader(networkHeader);
  String error;
'''
replacement = '''  Serial.printf("[PROBE][IPP-LIVE] start candidate=%d IF=%u ALT=%u OUT=0x%02X IN=0x%02X\\n",
                (int)selected, iface->interfaceNumber, iface->alternateSetting,
                iface->bulkOut.address, iface->bulkIn.address);

  String error;
  if (!usbHost.beginIppLiveIo(error)) {
    ippLiveLastError = error;
    client.print("HTTP/1.1 503 Service Unavailable\\r\\nConnection: close\\r\\nContent-Length: 0\\r\\n\\r\\n");
    client.stop();
    ippLiveActive = false;
    return;
  }

  const String usbHeader = normalizeLiveIppHeader(networkHeader);
'''
s = replace_once(s, needle, replacement, 'begin persistent session before header')

# Ensure every live-path early failure after session start stops persistent I/O.
s = replace_once(s,
'''  if (!liveIppUsbWrite((const uint8_t *)usbHeader.c_str(), usbHeader.length(), error)) {
    ippLiveLastError = error;
    client.stop();
    ippLiveActive = false;
    return;
  }
''',
'''  if (!liveIppUsbWrite((const uint8_t *)usbHeader.c_str(), usbHeader.length(), error)) {
    ippLiveLastError = error;
    usbHost.endIppLiveIo();
    client.stop();
    ippLiveActive = false;
    return;
  }
''', 'stop persistent session on header failure')

s = replace_once(s,
'''  client.stop();
  ippLiveActive = false;
}

void handleIppClient(WiFiClient client) {
''',
'''  usbHost.endIppLiveIo();
  client.stop();
  ippLiveActive = false;
}

void handleIppClient(WiFiClient client) {
''', 'stop persistent session at live close')

s = s.replace('protocol-0x04 Bulk-IN is polled between every small OUT transfer so 100-Continue, status and final responses can reach Android during the request.',
              'protocol-0x04 Bulk-IN stays persistently armed while a reusable Bulk-OUT transfer sends data, so 100-Continue, status and final responses can arrive even while an OUT transfer is in flight.', 1)

p.write_text(s)
print('Persistent asynchronous IPP IN + reusable OUT session patch applied')
