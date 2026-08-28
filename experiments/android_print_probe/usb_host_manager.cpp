#include "usb_host_manager.h"
#include "esp_intr_alloc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "usb/usb_host.h"
#include "usb/usb_helpers.h"
#include "usb/usb_types_ch9.h"
#include <cstring>

namespace {
static constexpr uint8_t USB_PRINTER_CLASS_CODE = 0x07;
static constexpr uint8_t USB_PRINTER_SUBCLASS_CODE = 0x01;
static constexpr uint8_t USB_ENDPOINT_XFER_BULK_CODE = 0x02;
static constexpr uint8_t USB_PRINTER_GET_PORT_STATUS = 0x01;
static constexpr uint8_t USB_PRINTER_STATUS_REQUEST_TYPE = 0xA1;
static constexpr int MAX_ALREADY_CONNECTED_DEVICES = 8;
static constexpr uint32_t STATUS_POLL_INTERVAL_MS = 1000;
static constexpr uint32_t STATUS_TRANSFER_TIMEOUT_MS = 1000;
static constexpr size_t IPP_LIVE_TRANSFER_BYTES = 1024;
static constexpr size_t IPP_LIVE_RX_STREAM_BYTES = 8192;
static constexpr uint32_t IPP_LIVE_IN_TIMEOUT_MS = 50;

struct TransferWait { SemaphoreHandle_t done = nullptr; };

struct Runtime {
  usb_host_client_handle_t client = nullptr;
  usb_device_handle_t device = nullptr;
  uint8_t address = 0;

  uint8_t claimedPrintInterface = 0;
  uint8_t claimedStatusInterface = 0;
  bool deviceOpen = false;
  bool printInterfaceClaimed = false;
  bool statusInterfaceClaimed = false;
  bool ippInterfaceClaimed = false;
  uint8_t claimedIppInterface = 0;

  volatile bool newDevice = false;
  volatile uint8_t newAddress = 0;
  volatile bool deviceGone = false;
  volatile bool statusPending = false;
  volatile usb_transfer_t *statusTransfer = nullptr;

  usb_transfer_t *ippLiveInTransfer = nullptr;
  usb_transfer_t *ippLiveOutTransfer = nullptr;
  SemaphoreHandle_t ippLiveOutDone = nullptr;
  SemaphoreHandle_t ippLiveInStopped = nullptr;
  StreamBufferHandle_t ippLiveRx = nullptr;
  volatile bool ippLiveRunning = false;
  volatile bool ippLiveInSubmitted = false;
  volatile int ippLiveErrorStatus = 0;

  TaskHandle_t clientTask = nullptr;
  TaskHandle_t hostTask = nullptr;
} g;

UsbHostManager *manager = nullptr;

#if defined(CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK)
static bool enumFilter(const usb_device_desc_t *, uint8_t *) { return true; }
#endif

static String usbString(const usb_str_desc_t *d) {
  if (!d || d->bLength < 2) return String();
  String s;
  const size_t n = (d->bLength - 2) / 2;
  s.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    uint16_t c = d->wData[i];
    if (!c) break;
    s += (char)(c <= 0x7F ? c : '?');
  }
  return s;
}

static bool bulk(const usb_ep_desc_t *e) {
  return e && ((e->bmAttributes & 0x03) == USB_ENDPOINT_XFER_BULK_CODE);
}

static void storeEndpoint(UsbEndpointInfo &slot, const usb_ep_desc_t *ept) {
  if (!ept || slot.valid()) return;
  slot.address = ept->bEndpointAddress;
  slot.attributes = ept->bmAttributes;
  slot.maxPacketSize = ept->wMaxPacketSize;
  slot.interval = ept->bInterval;
}

static int printScore(const UsbPrinterInterfaceInfo &p) {
  if (!p.usableForRawPrint()) return -1000;
  if (p.protocol == 0x02) return 120 + (p.bulkIn.valid() ? 10 : 0) + (p.alternateSetting == 0 ? 2 : 0);
  if (p.protocol == 0x03) return 90 + (p.bulkIn.valid() ? 10 : 0) + (p.alternateSetting == 0 ? 2 : 0);
  if (p.protocol == 0x01) return 70 + (p.alternateSetting == 0 ? 2 : 0);
  return -1000;
}

static int selectedPrintIndex(const UsbDeviceInfo &d) {
  int best = -1;
  int bestScore = -1001;
  for (uint8_t i = 0; i < d.printerInterfaceCount; ++i) {
    const int s = printScore(d.printerInterfaces[i]);
    if (s > bestScore) {
      bestScore = s;
      best = i;
    }
  }
  return best;
}

static int statusScore(const UsbPrinterInterfaceInfo &p) {
  if (!p.usableForStatus()) return -1000;
  int score = 10;
  if (p.bulkIn.valid()) score += 20;
  if (p.protocol == 0x03) score += 30;
  else if (p.protocol == 0x02) score += 20;
  else if (p.protocol == 0x01) score += 10;
  if (p.alternateSetting == 0) score += 3;
  return score;
}

static int selectedStatusIndex(const UsbDeviceInfo &d, int printIndex) {
  int best = -1;
  int bestScore = -1001;
  for (uint8_t i = 0; i < d.printerInterfaceCount; ++i) {
    const auto &p = d.printerInterfaces[i];
    if ((int)i == printIndex) continue;
    bool sameInterfaceAsPrint = false;
    if (printIndex >= 0) {
      sameInterfaceAsPrint = p.interfaceNumber == d.printerInterfaces[printIndex].interfaceNumber;
    }
    if (sameInterfaceAsPrint || !p.usableForStatus()) continue;
    const int s = statusScore(p);
    if (s > bestScore) {
      bestScore = s;
      best = i;
    }
  }
  return best;
}

static void resetDevice() {
  g.device = nullptr;
  g.address = 0;
  g.deviceOpen = false;
  g.printInterfaceClaimed = false;
  g.statusInterfaceClaimed = false;
  g.ippInterfaceClaimed = false;
  g.claimedPrintInterface = 0;
  g.claimedStatusInterface = 0;
  g.claimedIppInterface = 0;
  g.statusTransfer = nullptr;
}

static void releaseInterfaces() {
  if (manager) manager->endIppLiveIo();
  if (!g.deviceOpen || !g.device) return;
  if (g.ippInterfaceClaimed) {
    usb_host_interface_release(g.client, g.device, g.claimedIppInterface);
    g.ippInterfaceClaimed = false;
  }
  if (g.statusInterfaceClaimed) {
    usb_host_interface_release(g.client, g.device, g.claimedStatusInterface);
    g.statusInterfaceClaimed = false;
  }
  if (g.printInterfaceClaimed) {
    usb_host_interface_release(g.client, g.device, g.claimedPrintInterface);
    g.printInterfaceClaimed = false;
  }
}

static void closeDevice() {
  if (!g.deviceOpen || !g.device) {
    resetDevice();
    return;
  }
  releaseInterfaces();
  usb_host_device_close(g.client, g.device);
  resetDevice();
}

static bool claimInterfaces(UsbDeviceInfo &d, String &error) {
  const int printIndex = selectedPrintIndex(d);
  if (printIndex < 0) {
    error = "No RAW-compatible USB Printer Class interface";
    return false;
  }

  UsbPrinterInterfaceInfo &print = d.printerInterfaces[printIndex];
  esp_err_t e = usb_host_interface_claim(g.client, g.device,
                                          print.interfaceNumber,
                                          print.alternateSetting);
  if (e != ESP_OK) {
    error = String("print interface claim failed: ") + esp_err_to_name(e);
    return false;
  }
  g.claimedPrintInterface = print.interfaceNumber;
  g.printInterfaceClaimed = true;
  d.printer = print;

  const int statusIndex = selectedStatusIndex(d, printIndex);
  d.statusInterfaceSeparate = false;
  d.statusInterface = UsbPrinterInterfaceInfo{};

  if (statusIndex >= 0) {
    UsbPrinterInterfaceInfo &status = d.printerInterfaces[statusIndex];
    e = usb_host_interface_claim(g.client, g.device,
                                 status.interfaceNumber,
                                 status.alternateSetting);
    if (e == ESP_OK) {
      g.claimedStatusInterface = status.interfaceNumber;
      g.statusInterfaceClaimed = true;
      d.statusInterface = status;
      d.statusInterfaceSeparate = true;
    } else {
      Serial.printf("[USB] Status interface IF=%u ALT=%u could not be claimed: %s; using print interface for status\n",
                    status.interfaceNumber, status.alternateSetting,
                    esp_err_to_name(e));
    }
  }

  if (!d.statusInterfaceSeparate) d.statusInterface = d.printer;

  // Keep protocol-0x04 interfaces unclaimed while classic IF1 printing is in
  // use. selectIppInterface() can still claim one explicitly later. This keeps
  // the live IF1 Bulk-IN/Bulk-OUT bridge from spending a scarce HCD channel on
  // an unrelated idle interface.
  d.ippSelectedIndex = -1;

  Serial.printf("[USB] Selected Printer Class IF=%u ALT=%u protocol=0x%02X OUT=0x%02X IN=0x%02X\n",
                d.printer.interfaceNumber, d.printer.alternateSetting,
                d.printer.protocol, d.printer.bulkOut.address,
                d.printer.bulkIn.address);
  if (d.statusInterfaceSeparate) {
    Serial.printf("[USB] Status IF=%u ALT=%u protocol=0x%02X (separate)\n",
                  d.statusInterface.interfaceNumber,
                  d.statusInterface.alternateSetting,
                  d.statusInterface.protocol);
  }
  return true;
}

static bool enumerateDevice(uint8_t address, UsbDeviceInfo &out, String &error) {
  usb_device_handle_t dev = nullptr;
  const usb_device_desc_t *dd = nullptr;
  const usb_config_desc_t *cd = nullptr;

  esp_err_t e = usb_host_device_open(g.client, address, &dev);
  if (e != ESP_OK) {
    error = String("usb_host_device_open failed: ") + esp_err_to_name(e);
    return false;
  }

  g.device = dev;
  g.address = address;
  g.deviceOpen = true;
  out = UsbDeviceInfo{};
  out.attached = true;
  out.address = address;

  e = usb_host_get_device_descriptor(dev, &dd);
  if (e != ESP_OK || !dd) {
    error = String("device descriptor read failed: ") + esp_err_to_name(e);
    closeDevice();
    return false;
  }
  out.vid = dd->idVendor;
  out.pid = dd->idProduct;

  usb_device_info_t di{};
  if (usb_host_device_info(dev, &di) == ESP_OK) {
    out.manufacturer = usbString(di.str_desc_manufacturer);
    out.product = usbString(di.str_desc_product);
    out.serial = usbString(di.str_desc_serial_num);
  }

  e = usb_host_get_active_config_descriptor(dev, &cd);
  if (e != ESP_OK || !cd) {
    error = String("configuration descriptor read failed: ") + esp_err_to_name(e);
    closeDevice();
    return false;
  }
  out.configurationValue = cd->bConfigurationValue;

  int offset = 0;
  const uint16_t total = cd->wTotalLength;
  const usb_standard_desc_t *d = usb_parse_next_descriptor(
      (const usb_standard_desc_t *)cd, total, &offset);
  UsbPrinterInterfaceInfo *printerCurrent = nullptr;

  while (d) {
    if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
      const usb_intf_desc_t *i = (const usb_intf_desc_t *)d;
      printerCurrent = nullptr;

      if (i->bInterfaceClass == USB_PRINTER_CLASS_CODE &&
          i->bInterfaceSubClass == USB_PRINTER_SUBCLASS_CODE) {
        if (i->bInterfaceProtocol == 0xFF) {
          Serial.printf("[USB] Ignoring IF=%u ALT=%u: vendor-specific Printer Class protocol 0xFF is not verified\n",
                        i->bInterfaceNumber, i->bAlternateSetting);
        } else if (i->bInterfaceProtocol >= 0x01 && i->bInterfaceProtocol <= 0x04 &&
                   out.printerInterfaceCount < UsbDeviceInfo::MAX_PRINTER_INTERFACES) {
          printerCurrent = &out.printerInterfaces[out.printerInterfaceCount++];
          printerCurrent->found = true;
          printerCurrent->interfaceNumber = i->bInterfaceNumber;
          printerCurrent->alternateSetting = i->bAlternateSetting;
          printerCurrent->subclass = i->bInterfaceSubClass;
          printerCurrent->protocol = i->bInterfaceProtocol;
        }
      }
    } else if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT) {
      const usb_ep_desc_t *ept = (const usb_ep_desc_t *)d;
      if (printerCurrent && bulk(ept)) {
        if ((ept->bEndpointAddress & 0x80) != 0) storeEndpoint(printerCurrent->bulkIn, ept);
        else storeEndpoint(printerCurrent->bulkOut, ept);
      }
    }
    d = usb_parse_next_descriptor((const usb_standard_desc_t *)d, total, &offset);
  }

  if (!out.printerInterfaceCount) {
    error = "USB device has no RAW-compatible Printer Class interface";
    closeDevice();
    return false;
  }

  uint8_t rawNumber = 0;
  uint8_t ippNumber = 0;
  for (uint8_t i = 0; i < out.printerInterfaceCount; ++i) {
    const auto &p = out.printerInterfaces[i];
    if (p.usableForIppUsb()) {
      Serial.printf("[USB] IPP-over-USB candidate %u: IF=%u ALT=%u OUT=0x%02X IN=0x%02X\n",
                    ippNumber++, p.interfaceNumber, p.alternateSetting,
                    p.bulkOut.address, p.bulkIn.address);
    } else if (p.usableForRawPrint()) {
      Serial.printf("[USB] RAW candidate %u: IF=%u ALT=%u protocol=0x%02X OUT=0x%02X IN=0x%02X score=%d\n",
                    rawNumber++, p.interfaceNumber, p.alternateSetting, p.protocol,
                    p.bulkOut.address, p.bulkIn.address, printScore(p));
    }
  }

  if (!claimInterfaces(out, error)) {
    closeDevice();
    return false;
  }
  return true;
}

static void publish(const UsbDeviceInfo &d) {
  if (!manager) return;
  manager->onEnumerated(d);
  Serial.printf("[USB] Printer ready: %s VID=0x%04X PID=0x%04X address=%u\n",
                d.product.length() ? d.product.c_str() : "USB Printer",
                d.vid, d.pid, d.address);
}

static void scanExisting() {
  uint8_t addresses[MAX_ALREADY_CONNECTED_DEVICES] = {};
  int count = 0;
  if (usb_host_device_addr_list_fill(MAX_ALREADY_CONNECTED_DEVICES, addresses, &count) != ESP_OK) return;
  for (int i = 0; i < count && !g.deviceOpen; ++i) {
    if (manager) manager->onEnumerationStarted();
    UsbDeviceInfo d;
    String error;
    if (enumerateDevice(addresses[i], d, error)) {
      publish(d);
      return;
    }
    if (manager) manager->onEnumerationError(error);
    Serial.printf("[USB] Ignored address %u: %s\n", addresses[i], error.c_str());
  }
}

static void clientEvent(const usb_host_client_event_msg_t *event, void *) {
  if (!event) return;
  if (event->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
    if (!g.deviceOpen && !g.newDevice) {
      g.newAddress = event->new_dev.address;
      g.newDevice = true;
    }
  } else if (event->event == USB_HOST_CLIENT_EVENT_DEV_GONE &&
             g.deviceOpen && event->dev_gone.dev_hdl == g.device) {
    g.deviceGone = true;
  }
}

static void bulkTransferCallback(usb_transfer_t *t) {
  if (!t) return;
  TransferWait *w = static_cast<TransferWait *>(t->context);
  if (w && w->done) xSemaphoreGive(w->done);
}

static void ippLiveOutCallback(usb_transfer_t *t) {
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

static void statusTransferCallback(usb_transfer_t *t) {
  if (!t || !manager) return;
  const bool ok = t->status == USB_TRANSFER_STATUS_COMPLETED && t->actual_num_bytes >= 9;
  if (ok) {
    manager->onPortStatusTransfer(true, t->data_buffer[8], String());
  } else {
    manager->onPortStatusTransfer(false, 0,
                                  String("GET_PORT_STATUS transfer status=") + String((int)t->status));
  }
  g.statusTransfer = nullptr;
  usb_host_transfer_free(t);
}

static void submitPortStatusIfPending() {
  if (!g.statusPending || g.statusTransfer || !g.deviceOpen || !manager) return;
  if (!manager->device().attached || !manager->statusInterface()) return;
  g.statusPending = false;

  usb_transfer_t *t = nullptr;
  const esp_err_t e = usb_host_transfer_alloc(9, 0, &t);
  if (e != ESP_OK || !t) {
    manager->onPortStatusTransfer(false, 0,
                                  String("GET_PORT_STATUS transfer alloc failed: ") + esp_err_to_name(e));
    return;
  }

  usb_setup_packet_t *setup = reinterpret_cast<usb_setup_packet_t *>(t->data_buffer);
  setup->bmRequestType = USB_PRINTER_STATUS_REQUEST_TYPE;
  setup->bRequest = USB_PRINTER_GET_PORT_STATUS;
  setup->wValue = 0;
  setup->wIndex = manager->statusInterface()->interfaceNumber;
  setup->wLength = 1;

  t->num_bytes = 9;
  t->device_handle = g.device;
  t->callback = statusTransferCallback;
  t->context = nullptr;
  t->timeout_ms = STATUS_TRANSFER_TIMEOUT_MS;

  const esp_err_t submit = usb_host_transfer_submit_control(g.client, t);
  if (submit != ESP_OK) {
    usb_host_transfer_free(t);
    manager->onPortStatusTransfer(false, 0,
                                  String("GET_PORT_STATUS submit failed: ") + esp_err_to_name(submit));
    return;
  }
  g.statusTransfer = t;
}

static void clientTask(void *) {
  usb_host_client_config_t c{};
  c.is_synchronous = false;
  c.max_num_event_msg = 8;
  c.async.client_event_callback = clientEvent;
  c.async.callback_arg = nullptr;

  const esp_err_t e = usb_host_client_register(&c, &g.client);
  if (e != ESP_OK) {
    if (manager) manager->onEnumerationError(String("usb_host_client_register failed: ") + esp_err_to_name(e));
    vTaskDelete(nullptr);
    return;
  }

  scanExisting();
  while (true) {
    const esp_err_t eventErr = usb_host_client_handle_events(g.client, pdMS_TO_TICKS(100));
    if (eventErr != ESP_OK && eventErr != ESP_ERR_TIMEOUT && manager) {
      manager->onEnumerationError(String("USB client event handling failed: ") + esp_err_to_name(eventErr));
    }

    if (g.deviceGone) {
      g.deviceGone = false;
      closeDevice();
      if (manager) manager->onDetached();
    }

    submitPortStatusIfPending();

    if (g.newDevice && !g.deviceOpen) {
      const uint8_t address = g.newAddress;
      g.newDevice = false;
      if (manager) manager->onEnumerationStarted();
      UsbDeviceInfo d;
      String error;
      if (enumerateDevice(address, d, error)) {
        publish(d);
      } else {
        if (manager) manager->onEnumerationError(error);
        Serial.printf("[USB] Ignored address %u: %s\n", address, error.c_str());
      }
    }
  }
}

static void hostTask(void *) {
  while (true) {
    uint32_t flags = 0;
    if (usb_host_lib_handle_events(portMAX_DELAY, &flags) != ESP_OK) {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }
}

} // namespace

bool UsbHostManager::begin() {
  if (started_) return state_ != ERROR;
  started_ = true;
  manager = this;

  usb_host_config_t hc{};
  hc.skip_phy_setup = false;
  hc.intr_flags = ESP_INTR_FLAG_LEVEL1;
#if defined(CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK)
  hc.enum_filter_cb = enumFilter;
#endif

  const esp_err_t e = usb_host_install(&hc);
  if (e != ESP_OK) {
    state_ = ERROR;
    error_ = String("usb_host_install failed: ") + esp_err_to_name(e);
    return false;
  }

  if (xTaskCreate(hostTask, "usb_host_lib", 4096, nullptr, 2, &g.hostTask) != pdPASS) {
    usb_host_uninstall();
    state_ = ERROR;
    error_ = "Failed to create USB host library task";
    return false;
  }
  if (xTaskCreate(clientTask, "usb_printer_host", 8192, nullptr, 3, &g.clientTask) != pdPASS) {
    vTaskDelete(g.hostTask);
    g.hostTask = nullptr;
    usb_host_uninstall();
    state_ = ERROR;
    error_ = "Failed to create USB printer client task";
    return false;
  }

  state_ = RUNNING;
  error_.clear();
  return true;
}

void UsbHostManager::poll() {
  static unsigned long lastStatusRequest = 0;
  if (state_ != PRINTER_READY || !device_.attached || !device_.statusInterface.found) return;
  if (millis() - lastStatusRequest < STATUS_POLL_INTERVAL_MS) return;
  lastStatusRequest = millis();
  g.statusPending = true;
}

const UsbPrinterInterfaceInfo *UsbHostManager::selectedInterface() const {
  return device_.printer.found ? &device_.printer : nullptr;
}

const UsbPrinterInterfaceInfo *UsbHostManager::statusInterface() const {
  return device_.statusInterface.found ? &device_.statusInterface : nullptr;
}


uint8_t UsbHostManager::ippInterfaceCount() const {
  uint8_t count = 0;
  for (uint8_t i = 0; i < device_.printerInterfaceCount; ++i) {
    if (device_.printerInterfaces[i].usableForIppUsb()) ++count;
  }
  return count;
}

const UsbPrinterInterfaceInfo *UsbHostManager::ippInterfaceAt(uint8_t index) const {
  uint8_t seen = 0;
  for (uint8_t i = 0; i < device_.printerInterfaceCount; ++i) {
    const auto &p = device_.printerInterfaces[i];
    if (!p.usableForIppUsb()) continue;
    if (seen == index) return &p;
    ++seen;
  }
  return nullptr;
}

bool UsbHostManager::selectIppInterface(uint8_t index, String &error) {
  const UsbPrinterInterfaceInfo *target = ippInterfaceAt(index);
  if (!target) {
    error = "Invalid IPP-over-USB interface index";
    return false;
  }
  if (!g.deviceOpen || !g.device) {
    error = "USB device is not open";
    return false;
  }
  if (target->interfaceNumber == device_.printer.interfaceNumber) {
    error = "IPP-over-USB candidate conflicts with active RAW interface";
    return false;
  }
  if (device_.ippSelectedIndex == (int8_t)index && g.ippInterfaceClaimed) return true;

  endIppLiveIo();
  if (g.ippInterfaceClaimed) {
    usb_host_interface_release(g.client, g.device, g.claimedIppInterface);
    g.ippInterfaceClaimed = false;
    device_.ippSelectedIndex = -1;
  }

  const esp_err_t e = usb_host_interface_claim(g.client, g.device,
                                                target->interfaceNumber,
                                                target->alternateSetting);
  if (e != ESP_OK) {
    error = String("IPP-over-USB interface claim failed: ") + esp_err_to_name(e);
    return false;
  }
  g.claimedIppInterface = target->interfaceNumber;
  g.ippInterfaceClaimed = true;
  device_.ippSelectedIndex = (int8_t)index;
  Serial.printf("[USB] Selected IPP-over-USB candidate %u IF=%u ALT=%u OUT=0x%02X IN=0x%02X\n",
                index, target->interfaceNumber, target->alternateSetting,
                target->bulkOut.address, target->bulkIn.address);
  return true;
}

bool UsbHostManager::bulkWrite(const uint8_t *data, size_t length, size_t &accepted,
                               uint32_t timeoutMs, String &error) {
  accepted = 0;
  if (!data || !length) {
    error = "empty USB transfer";
    return false;
  }
  if (!g.deviceOpen || !g.printInterfaceClaimed || !device_.printer.usableForRawPrint()) {
    error = "USB printer interface is not claimed/ready";
    return false;
  }
  if (length > 16384) {
    error = "USB transfer chunk exceeds 16 KiB";
    return false;
  }

  usb_transfer_t *t = nullptr;
  esp_err_t e = usb_host_transfer_alloc(length, 0, &t);
  if (e != ESP_OK || !t) {
    error = String("usb_host_transfer_alloc failed: ") + esp_err_to_name(e);
    return false;
  }

  TransferWait w;
  w.done = xSemaphoreCreateBinary();
  if (!w.done) {
    usb_host_transfer_free(t);
    error = "unable to allocate transfer completion semaphore";
    return false;
  }

  memcpy(t->data_buffer, data, length);
  t->num_bytes = length;
  t->device_handle = g.device;
  t->bEndpointAddress = device_.printer.bulkOut.address;
  t->callback = bulkTransferCallback;
  t->context = &w;
  t->timeout_ms = timeoutMs;

  e = usb_host_transfer_submit(t);
  if (e != ESP_OK) {
    vSemaphoreDelete(w.done);
    usb_host_transfer_free(t);
    error = String("usb_host_transfer_submit failed: ") + esp_err_to_name(e);
    return false;
  }

  const TickType_t waitTicks = timeoutMs ? pdMS_TO_TICKS(timeoutMs + 250) : portMAX_DELAY;
  if (xSemaphoreTake(w.done, waitTicks) != pdTRUE) {
    usb_host_endpoint_halt(t->device_handle, t->bEndpointAddress);
    usb_host_endpoint_flush(t->device_handle, t->bEndpointAddress);
    usb_host_endpoint_clear(t->device_handle, t->bEndpointAddress);
    xSemaphoreTake(w.done, portMAX_DELAY);
  }

  accepted = static_cast<size_t>(t->actual_num_bytes);
  const usb_transfer_status_t status = t->status;
  vSemaphoreDelete(w.done);
  usb_host_transfer_free(t);

  if (status != USB_TRANSFER_STATUS_COMPLETED || accepted != length) {
    error = String("USB bulk transfer failed status=") + String((int)status) +
            " accepted=" + String((unsigned)accepted) + "/" + String((unsigned)length);
    return false;
  }
  return true;
}


bool UsbHostManager::bulkRead(uint8_t *data, size_t capacity, size_t &received,
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

bool UsbHostManager::ippBulkWrite(const uint8_t *data, size_t length, size_t &accepted,
                                  uint32_t timeoutMs, String &error) {
  accepted = 0;
  const int8_t selected = device_.ippSelectedIndex;
  const UsbPrinterInterfaceInfo *iface = selected >= 0 ? ippInterfaceAt((uint8_t)selected) : nullptr;
  if (!data || !length) { error = "empty IPP-over-USB transfer"; return false; }
  if (!g.deviceOpen || !g.ippInterfaceClaimed || !iface || !iface->usableForIppUsb()) {
    error = "IPP-over-USB interface is not claimed/ready";
    return false;
  }
  if (length > 16384) { error = "IPP-over-USB transfer chunk exceeds 16 KiB"; return false; }

  usb_transfer_t *t = nullptr;
  esp_err_t e = usb_host_transfer_alloc(length, 0, &t);
  if (e != ESP_OK || !t) { error = String("usb_host_transfer_alloc failed: ") + esp_err_to_name(e); return false; }
  TransferWait w;
  w.done = xSemaphoreCreateBinary();
  if (!w.done) { usb_host_transfer_free(t); error = "unable to allocate transfer completion semaphore"; return false; }

  memcpy(t->data_buffer, data, length);
  t->num_bytes = length;
  t->device_handle = g.device;
  t->bEndpointAddress = iface->bulkOut.address;
  t->callback = bulkTransferCallback;
  t->context = &w;
  t->timeout_ms = timeoutMs;

  e = usb_host_transfer_submit(t);
  if (e != ESP_OK) {
    vSemaphoreDelete(w.done); usb_host_transfer_free(t);
    error = String("IPP-over-USB OUT submit failed: ") + esp_err_to_name(e);
    return false;
  }
  const TickType_t waitTicks = timeoutMs ? pdMS_TO_TICKS(timeoutMs + 250) : portMAX_DELAY;
  if (xSemaphoreTake(w.done, waitTicks) != pdTRUE) {
    usb_host_endpoint_halt(t->device_handle, t->bEndpointAddress);
    usb_host_endpoint_flush(t->device_handle, t->bEndpointAddress);
    usb_host_endpoint_clear(t->device_handle, t->bEndpointAddress);
    xSemaphoreTake(w.done, portMAX_DELAY);
  }
  accepted = static_cast<size_t>(t->actual_num_bytes);
  const usb_transfer_status_t status = t->status;
  vSemaphoreDelete(w.done); usb_host_transfer_free(t);
  if (status != USB_TRANSFER_STATUS_COMPLETED || accepted != length) {
    error = String("IPP-over-USB OUT failed status=") + String((int)status) +
            " accepted=" + String((unsigned)accepted) + "/" + String((unsigned)length);
    return false;
  }
  return true;
}

bool UsbHostManager::ippBulkRead(uint8_t *data, size_t capacity, size_t &received,
                                 uint32_t timeoutMs, String &error) {
  received = 0;
  const int8_t selected = device_.ippSelectedIndex;
  const UsbPrinterInterfaceInfo *iface = selected >= 0 ? ippInterfaceAt((uint8_t)selected) : nullptr;
  if (!data || !capacity) { error = "empty IPP-over-USB read buffer"; return false; }
  if (!g.deviceOpen || !g.ippInterfaceClaimed || !iface || !iface->usableForIppUsb()) {
    error = "IPP-over-USB interface is not claimed/ready";
    return false;
  }
  if (capacity > 16384) capacity = 16384;

  usb_transfer_t *t = nullptr;
  esp_err_t e = usb_host_transfer_alloc(capacity, 0, &t);
  if (e != ESP_OK || !t) { error = String("usb_host_transfer_alloc failed: ") + esp_err_to_name(e); return false; }
  TransferWait w;
  w.done = xSemaphoreCreateBinary();
  if (!w.done) { usb_host_transfer_free(t); error = "unable to allocate transfer completion semaphore"; return false; }

  t->num_bytes = capacity;
  t->device_handle = g.device;
  t->bEndpointAddress = iface->bulkIn.address;
  t->callback = bulkTransferCallback;
  t->context = &w;
  t->timeout_ms = timeoutMs;

  e = usb_host_transfer_submit(t);
  if (e != ESP_OK) {
    vSemaphoreDelete(w.done); usb_host_transfer_free(t);
    error = String("IPP-over-USB IN submit failed: ") + esp_err_to_name(e);
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
  if (status == USB_TRANSFER_STATUS_COMPLETED && received) memcpy(data, t->data_buffer, received);
  vSemaphoreDelete(w.done); usb_host_transfer_free(t);
  if (status != USB_TRANSFER_STATUS_COMPLETED || received == 0) {
    error = String("IPP-over-USB IN failed status=") + String((int)status) +
            " received=" + String((unsigned)received);
    return false;
  }
  return true;
}

bool UsbHostManager::beginIppLiveIo(String &error) {
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

bool UsbHostManager::ippBulkReadPoll(uint8_t *data, size_t capacity, size_t &received,
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

void UsbHostManager::onPortStatusTransfer(bool valid, uint8_t value, const String &error) {
  static String lastStatusError;
  if (!valid) {
    if (error.length() && error != lastStatusError) {
      Serial.printf("[USB] GET_PORT_STATUS error: %s\n", error.c_str());
      lastStatusError = error;
    }
    if (error.length()) error_ = error;
    return;
  }

  const bool wasValid = device_.portStatus.valid;
  const uint8_t previousValue = device_.portStatus.value;
  UsbPortStatus &s = device_.portStatus;
  s.valid = true;
  s.value = value;
  s.error = (value & 0x08) == 0;
  s.selected = (value & 0x10) != 0;
  s.paperEmpty = (value & 0x20) != 0;
  s.updatedAt = millis();
  error_.clear();
  lastStatusError.clear();

  if (!wasValid || previousValue != value) {
    Serial.printf("[USB] GET_PORT_STATUS IF=%u value=0x%02X error=%d selected=%d paper-empty=%d\n",
                  device_.statusInterface.interfaceNumber, value,
                  (int)s.error, (int)s.selected, (int)s.paperEmpty);
  }
}

void UsbHostManager::onEnumerationStarted() {
  if (!started_) return;
  state_ = ENUMERATING;
  error_.clear();
}

void UsbHostManager::onEnumerated(const UsbDeviceInfo &info) {
  device_ = info;
  error_.clear();
  state_ = info.printer.found ? PRINTER_READY : DEVICE_ATTACHED;
}

void UsbHostManager::onDetached() {
  if (device_.attached) Serial.println("[USB] Device disconnected");
  device_ = UsbDeviceInfo{};
  error_.clear();
  state_ = started_ ? RUNNING : STOPPED;
}

void UsbHostManager::onEnumerationError(const String &error) {
  error_ = error;
  if (started_ && state_ != ERROR) state_ = RUNNING;
}
