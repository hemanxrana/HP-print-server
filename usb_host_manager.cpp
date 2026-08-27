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
static constexpr size_t BACKCHANNEL_BUFFER_BYTES = 4096;

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

  volatile bool newDevice = false;
  volatile uint8_t newAddress = 0;
  volatile bool deviceGone = false;
  volatile bool statusPending = false;
  volatile usb_transfer_t *statusTransfer = nullptr;

  // One continuously outstanding Bulk-IN transfer provides a true printer
  // backchannel without changing the existing synchronous Bulk-OUT path.
  usb_transfer_t *backchannelTransfer = nullptr;
  StreamBufferHandle_t backchannelBuffer = nullptr;
  volatile bool backchannelEnabled = false;
  volatile uint32_t backchannelDropped = 0;
  uint8_t backchannelEndpoint = 0;
  uint16_t backchannelPacketSize = 0;

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

static void resetBackchannelBuffer() {
  if (g.backchannelBuffer) xStreamBufferReset(g.backchannelBuffer);
  g.backchannelDropped = 0;
}

static void resetDevice() {
  g.device = nullptr;
  g.address = 0;
  g.deviceOpen = false;
  g.printInterfaceClaimed = false;
  g.statusInterfaceClaimed = false;
  g.claimedPrintInterface = 0;
  g.claimedStatusInterface = 0;
  g.statusTransfer = nullptr;
  g.backchannelEnabled = false;
  g.backchannelEndpoint = 0;
  g.backchannelPacketSize = 0;
  resetBackchannelBuffer();
}

static void releaseInterfaces() {
  if (!g.deviceOpen || !g.device) return;
  g.backchannelEnabled = false;
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
        if (i->bInterfaceProtocol == 0x04) {
          Serial.printf("[USB] Ignoring IF=%u ALT=%u: protocol 0x04 is IPP-over-USB, not RAW printing\n",
                        i->bInterfaceNumber, i->bAlternateSetting);
        } else if (i->bInterfaceProtocol == 0xFF) {
          Serial.printf("[USB] Ignoring IF=%u ALT=%u: vendor-specific Printer Class protocol 0xFF is not verified for RAW\n",
                        i->bInterfaceNumber, i->bAlternateSetting);
        } else if (i->bInterfaceProtocol >= 0x01 && i->bInterfaceProtocol <= 0x03 &&
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

  for (uint8_t i = 0; i < out.printerInterfaceCount; ++i) {
    const auto &p = out.printerInterfaces[i];
    Serial.printf("[USB] RAW candidate %u: IF=%u ALT=%u protocol=0x%02X OUT=0x%02X IN=0x%02X score=%d\n",
                  i, p.interfaceNumber, p.alternateSetting, p.protocol,
                  p.bulkOut.address, p.bulkIn.address, printScore(p));
  }

  if (!claimInterfaces(out, error)) {
    closeDevice();
    return false;
  }
  return true;
}

static void backchannelTransferCallback(usb_transfer_t *t) {
  if (!t) return;

  const bool stillActive = g.backchannelEnabled && g.deviceOpen &&
      g.printInterfaceClaimed && g.device == t->device_handle &&
      g.backchannelEndpoint == t->bEndpointAddress;

  if (t->status == USB_TRANSFER_STATUS_COMPLETED) {
    if (t->actual_num_bytes > 0 && g.backchannelBuffer) {
      const size_t pushed = xStreamBufferSend(g.backchannelBuffer,
                                               t->data_buffer,
                                               t->actual_num_bytes,
                                               0);
      if (pushed < t->actual_num_bytes) {
        g.backchannelDropped += (uint32_t)(t->actual_num_bytes - pushed);
      }
    }

    if (stillActive) {
      t->num_bytes = g.backchannelPacketSize;
      const esp_err_t submit = usb_host_transfer_submit(t);
      if (submit == ESP_OK) return;
      Serial.printf("[USB] Backchannel resubmit failed: %s; continuing print-only\n",
                    esp_err_to_name(submit));
    }
  } else if (t->status != USB_TRANSFER_STATUS_NO_DEVICE &&
             t->status != USB_TRANSFER_STATUS_CANCELED) {
    Serial.printf("[USB] Backchannel disabled after transfer status=%d; continuing print-only\n",
                  (int)t->status);
  }

  g.backchannelEnabled = false;
  g.backchannelTransfer = nullptr;
  usb_host_transfer_free(t);
}

static void startBackchannel() {
  if (!manager || !g.deviceOpen || !g.printInterfaceClaimed) return;
  const UsbPrinterInterfaceInfo *print = manager->selectedInterface();
  if (!print || !print->bulkIn.valid()) {
    Serial.println("[USB] Printer has no Bulk IN backchannel; RAW printing remains one-way");
    return;
  }

  if (g.backchannelTransfer) {
    Serial.println("[USB] Previous Bulk IN transfer is still finalizing; backchannel remains disabled for this attachment");
    return;
  }

  if (!g.backchannelBuffer) {
    g.backchannelBuffer = xStreamBufferCreate(BACKCHANNEL_BUFFER_BYTES, 1);
    if (!g.backchannelBuffer) {
      Serial.println("[USB] Could not allocate backchannel buffer; continuing print-only");
      return;
    }
  }
  resetBackchannelBuffer();

  const uint16_t packetSize = print->bulkIn.maxPacketSize ? print->bulkIn.maxPacketSize : 64;
  usb_transfer_t *t = nullptr;
  const esp_err_t alloc = usb_host_transfer_alloc(packetSize, 0, &t);
  if (alloc != ESP_OK || !t) {
    Serial.printf("[USB] Could not allocate Bulk IN backchannel transfer: %s; continuing print-only\n",
                  esp_err_to_name(alloc));
    return;
  }

  g.backchannelEndpoint = print->bulkIn.address;
  g.backchannelPacketSize = packetSize;
  g.backchannelEnabled = true;
  g.backchannelTransfer = t;

  t->num_bytes = packetSize;
  t->device_handle = g.device;
  t->bEndpointAddress = g.backchannelEndpoint;
  t->callback = backchannelTransferCallback;
  t->context = nullptr;
  t->timeout_ms = 0;

  const esp_err_t submit = usb_host_transfer_submit(t);
  if (submit != ESP_OK) {
    g.backchannelEnabled = false;
    g.backchannelTransfer = nullptr;
    usb_host_transfer_free(t);
    Serial.printf("[USB] Could not start Bulk IN backchannel: %s; continuing print-only\n",
                  esp_err_to_name(submit));
    return;
  }

  Serial.printf("[USB] Full-duplex backchannel enabled on IN=0x%02X MPS=%u buffer=%u bytes\n",
                g.backchannelEndpoint, g.backchannelPacketSize,
                (unsigned)BACKCHANNEL_BUFFER_BYTES);
}

static void publish(const UsbDeviceInfo &d) {
  if (!manager) return;
  manager->onEnumerated(d);
  startBackchannel();
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
      g.backchannelEnabled = false;
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

bool UsbHostManager::backchannelSupported() const {
  return device_.printer.found && device_.printer.bulkIn.valid() && g.backchannelEnabled;
}

size_t UsbHostManager::backchannelAvailable() const {
  return g.backchannelBuffer ? xStreamBufferBytesAvailable(g.backchannelBuffer) : 0;
}

size_t UsbHostManager::readBackchannel(uint8_t *data, size_t capacity) {
  if (!data || !capacity || !g.backchannelBuffer) return 0;
  return xStreamBufferReceive(g.backchannelBuffer, data, capacity, 0);
}

void UsbHostManager::clearBackchannel() {
  if (g.backchannelBuffer) xStreamBufferReset(g.backchannelBuffer);
}

uint32_t UsbHostManager::backchannelDroppedBytes() const {
  return g.backchannelDropped;
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
  clearBackchannel();
  error_.clear();
  state_ = started_ ? RUNNING : STOPPED;
}

void UsbHostManager::onEnumerationError(const String &error) {
  error_ = error;
  if (started_ && state_ != ERROR) state_ = RUNNING;
}
