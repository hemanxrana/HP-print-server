#include "usb_host_manager.h"
#include "esp_intr_alloc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/usb_host.h"
#include "usb/usb_helpers.h"
#include "usb/usb_types_ch9.h"
#include <cstring>

namespace {
static constexpr uint8_t USB_PRINTER_CLASS_CODE = 0x07;
static constexpr uint8_t USB_PRINTER_SUBCLASS_CODE = 0x01;
static constexpr uint8_t USB_ENDPOINT_XFER_BULK_CODE = 0x02;
static constexpr int MAX_ALREADY_CONNECTED_DEVICES = 8;
static constexpr uint32_t STATUS_TIMEOUT_MS = 1000;

struct TransferWait { SemaphoreHandle_t done = nullptr; };
struct Runtime {
  usb_host_client_handle_t client = nullptr;
  usb_device_handle_t device = nullptr;
  uint8_t address = 0;
  uint8_t claimedInterface = 0;
  bool deviceOpen = false;
  bool interfaceClaimed = false;
  volatile bool newDevice = false;
  volatile uint8_t newAddress = 0;
  volatile bool deviceGone = false;
  volatile usb_device_handle_t goneHandle = nullptr;
  volatile bool selectionPending = false;
  volatile bool statusPending = false;
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

static int score(const UsbPrinterInterfaceInfo &p) {
  if (!p.usableForRawPrint()) return -1000;
  if (p.protocol == 0x02) return 112 + (p.bulkIn.valid() ? 10 : 0) + (p.alternateSetting == 0 ? 2 : 0);
  if (p.protocol == 0x01 || p.protocol == 0x03) return 72 + (p.bulkIn.valid() ? 10 : 0);
  if (p.protocol == 0x04) return 82 + (p.bulkIn.valid() ? 10 : 0);
  if (p.protocol == 0xFF) return 20;
  return 10;
}

static int selectedIndex(const UsbDeviceInfo &d) {
  int best = -1, bestScore = -1001;
  if (!manager) return -1;
  for (uint8_t i = 0; i < d.printerInterfaceCount; ++i) {
    const auto &p = d.printerInterfaces[i];
    if (!p.usableForRawPrint()) continue;
    if (!manager->automaticInterfaceSelection()) {
      if (p.interfaceNumber == manager->manualInterfaceNumber() &&
          p.alternateSetting == manager->manualAlternateSetting()) return i;
    } else {
      const int s = score(p);
      if (s > bestScore) { bestScore = s; best = i; }
    }
  }
  return best;
}

static void resetDevice() {
  g.device = nullptr;
  g.address = 0;
  g.deviceOpen = false;
  g.interfaceClaimed = false;
  g.statusPending = false;
}

static void closeDevice() {
  if (!g.deviceOpen || !g.device) { resetDevice(); return; }
  if (g.interfaceClaimed) {
    usb_host_interface_release(g.client, g.device, g.claimedInterface);
    g.interfaceClaimed = false;
  }
  usb_host_device_close(g.client, g.device);
  resetDevice();
}

static bool claim(UsbDeviceInfo &d, String &error) {
  const int i = selectedIndex(d);
  if (i < 0) {
    error = manager && manager->automaticInterfaceSelection()
              ? "No usable Printer Class Bulk OUT interface"
              : "Configured printer interface not found";
    return false;
  }
  UsbPrinterInterfaceInfo &p = d.printerInterfaces[i];
  const esp_err_t e = usb_host_interface_claim(g.client, g.device,
                                                p.interfaceNumber,
                                                p.alternateSetting);
  if (e != ESP_OK) {
    error = String("usb_host_interface_claim failed: ") + esp_err_to_name(e);
    return false;
  }
  g.claimedInterface = p.interfaceNumber;
  g.interfaceClaimed = true;
  d.printer = p;
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
  UsbPrinterInterfaceInfo *current = nullptr;

  while (d) {
    if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
      const usb_intf_desc_t *i = (const usb_intf_desc_t *)d;
      current = nullptr;
      if (i->bInterfaceClass == USB_PRINTER_CLASS_CODE &&
          i->bInterfaceSubClass == USB_PRINTER_SUBCLASS_CODE &&
          ((i->bInterfaceProtocol >= 1 && i->bInterfaceProtocol <= 4) ||
           i->bInterfaceProtocol == 0xFF) &&
          out.printerInterfaceCount < UsbDeviceInfo::MAX_PRINTER_INTERFACES) {
        current = &out.printerInterfaces[out.printerInterfaceCount++];
        current->found = true;
        current->interfaceNumber = i->bInterfaceNumber;
        current->alternateSetting = i->bAlternateSetting;
        current->subclass = i->bInterfaceSubClass;
        current->protocol = i->bInterfaceProtocol;
      }
    } else if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT &&
               current && bulk((const usb_ep_desc_t *)d)) {
      const usb_ep_desc_t *ept = (const usb_ep_desc_t *)d;
      UsbEndpointInfo ep;
      ep.address = ept->bEndpointAddress;
      ep.attributes = ept->bmAttributes;
      ep.maxPacketSize = ept->wMaxPacketSize;
      ep.interval = ept->bInterval;
      if (ep.isIn()) {
        if (!current->bulkIn.valid()) current->bulkIn = ep;
      } else if (!current->bulkOut.valid()) {
        current->bulkOut = ep;
      }
    }
    d = usb_parse_next_descriptor((const usb_standard_desc_t *)d, total, &offset);
  }

  if (!out.printerInterfaceCount) {
    error = "USB device has no USB Printer Class interface";
    closeDevice();
    return false;
  }
  if (!claim(out, error)) {
    closeDevice();
    return false;
  }

  // Prefer a distinct IEEE 1284.4/status-capable Printer Class interface.
  // GET_PORT_STATUS is a class control request, not WinUSB/vendor REST.
  for (uint8_t i = 0; i < out.printerInterfaceCount; ++i) {
    const auto &p = out.printerInterfaces[i];
    if (p.interfaceNumber == out.printer.interfaceNumber &&
        p.alternateSetting == out.printer.alternateSetting) continue;
    if (p.protocol == 0x04 || p.protocol == 0x03 || p.protocol == 0x02) {
      out.statusInterfaceFound = true;
      out.statusInterfaceNumber = p.interfaceNumber;
      out.statusAlternateSetting = p.alternateSetting;
      break;
    }
  }
  if (!out.statusInterfaceFound) {
    out.statusInterfaceFound = true;
    out.statusInterfaceNumber = out.printer.interfaceNumber;
    out.statusAlternateSetting = out.printer.alternateSetting;
  }

  for (uint8_t i = 0; i < out.printerInterfaceCount; ++i) {
    const auto &p = out.printerInterfaces[i];
    Serial.printf("[USB] Candidate %u: IF=%u ALT=%u protocol=0x%02X OUT=0x%02X IN=0x%02X score=%d\n",
                  i, p.interfaceNumber, p.alternateSetting, p.protocol,
                  p.bulkOut.address, p.bulkIn.address, score(p));
  }
  Serial.printf("[USB] STATUS interface IF=%u ALT=%u\n",
                out.statusInterfaceNumber, out.statusAlternateSetting);
  return true;
}

static bool readPortStatus(uint8_t interfaceNumber, uint8_t &status, String &error) {
  if (!g.deviceOpen || !g.device || !g.client) {
    error = "USB device is not open";
    return false;
  }

  usb_transfer_t *t = nullptr;
  const esp_err_t alloc = usb_host_transfer_alloc(9, 0, &t);
  if (alloc != ESP_OK || !t) {
    error = String("status transfer alloc failed: ") + esp_err_to_name(alloc);
    return false;
  }

  TransferWait w;
  w.done = xSemaphoreCreateBinary();
  if (!w.done) {
    usb_host_transfer_free(t);
    error = "unable to allocate status transfer semaphore";
    return false;
  }

  usb_setup_packet_t setup{};
  setup.bmRequestType = 0xA1; // device-to-host | class | interface
  setup.bRequest = 0x01;      // USB Printer Class GET_PORT_STATUS
  setup.wValue = 0;
  setup.wIndex = interfaceNumber;
  setup.wLength = 1;
  memcpy(t->data_buffer, setup.val, USB_SETUP_PACKET_SIZE);
  t->data_buffer[8] = 0;
  t->num_bytes = 9;
  t->device_handle = g.device;
  t->callback = transferCallback;
  t->context = &w;
  t->timeout_ms = STATUS_TIMEOUT_MS;

  const esp_err_t submit = usb_host_transfer_submit_control(g.client, t);
  if (submit != ESP_OK) {
    vSemaphoreDelete(w.done);
    usb_host_transfer_free(t);
    error = String("GET_PORT_STATUS submit failed: ") + esp_err_to_name(submit);
    return false;
  }

  const TickType_t waitTicks = pdMS_TO_TICKS(STATUS_TIMEOUT_MS + 250);
  if (xSemaphoreTake(w.done, waitTicks) != pdTRUE) {
    error = "GET_PORT_STATUS timed out";
    xSemaphoreTake(w.done, portMAX_DELAY);
  }

  const usb_transfer_status_t transferStatus = t->status;
  const size_t actual = static_cast<size_t>(t->actual_num_bytes);
  if (transferStatus == USB_TRANSFER_STATUS_COMPLETED && actual >= 9) {
    status = t->data_buffer[8];
    vSemaphoreDelete(w.done);
    usb_host_transfer_free(t);
    return true;
  }

  error = String("GET_PORT_STATUS failed status=") + String((int)transferStatus) +
          " bytes=" + String((unsigned)actual);
  vSemaphoreDelete(w.done);
  usb_host_transfer_free(t);
  return false;
}

static void refreshStatusIfPending() {
  if (!g.statusPending || !g.deviceOpen || !manager) return;
  g.statusPending = false;
  UsbDeviceInfo d = manager->device();
  if (!d.statusInterfaceFound) return;
  uint8_t status = 0;
  String error;
  if (readPortStatus(d.statusInterfaceNumber, status, error)) {
    d.portStatus = status;
    d.portStatusValid = true;
    manager->onEnumerated(d);
    Serial.printf("[USB] GET_PORT_STATUS IF=%u -> 0x%02X online=%s paper=%s error=%s\n",
                  d.statusInterfaceNumber, status,
                  (status & 0x08) ? "yes" : "no",
                  (status & 0x20) ? "yes" : "no",
                  (status & 0x40) ? "yes" : "no");
  } else {
    d.portStatusValid = false;
    manager->onEnumerated(d);
    Serial.printf("[USB] GET_PORT_STATUS unavailable: %s\n", error.c_str());
  }
}

static void publish(const UsbDeviceInfo &d) {
  if (!manager) return;
  manager->onEnumerated(d);
  Serial.printf("[USB] %s VID=0x%04X PID=0x%04X address=%u\n",
                d.product.length() ? d.product.c_str() : "USB Printer",
                d.vid, d.pid, d.address);
  Serial.printf("[USB] SELECTED IF=%u ALT=%u protocol=0x%02X OUT=0x%02X IN=0x%02X\n",
                d.printer.interfaceNumber, d.printer.alternateSetting,
                d.printer.protocol, d.printer.bulkOut.address,
                d.printer.bulkIn.address);
}

static void scanExisting() {
  uint8_t addresses[MAX_ALREADY_CONNECTED_DEVICES] = {};
  int count = 0;
  if (usb_host_device_addr_list_fill(MAX_ALREADY_CONNECTED_DEVICES, addresses, &count) != ESP_OK) return;
  for (int i = 0; i < count && !g.deviceOpen; ++i) {
    UsbDeviceInfo d;
    String error;
    if (enumerateDevice(addresses[i], d, error)) {
      publish(d);
      return;
    }
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
    g.goneHandle = event->dev_gone.dev_hdl;
    g.deviceGone = true;
  }
}

static void applySelectionIfPending() {
  if (!g.selectionPending || !g.deviceOpen || !manager) return;
  g.selectionPending = false;
  UsbDeviceInfo d = manager->device();
  const int i = selectedIndex(d);
  if (i < 0) {
    manager->onEnumerationError("Configured printer interface not found");
    return;
  }
  if (d.printer.interfaceNumber == d.printerInterfaces[i].interfaceNumber &&
      d.printer.alternateSetting == d.printerInterfaces[i].alternateSetting) return;

  if (g.interfaceClaimed) {
    usb_host_interface_release(g.client, g.device, g.claimedInterface);
    g.interfaceClaimed = false;
  }
  String error;
  if (!claim(d, error)) {
    manager->onEnumerationError(error);
    return;
  }
  publish(d);
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
    applySelectionIfPending();
    refreshStatusIfPending();
    if (g.newDevice && !g.deviceOpen) {
      const uint8_t address = g.newAddress;
      g.newDevice = false;
      if (manager) manager->onEnumerationError("enumerating-usb-device");
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

static void transferCallback(usb_transfer_t *t) {
  if (!t) return;
  TransferWait *w = static_cast<TransferWait *>(t->context);
  if (w && w->done) xSemaphoreGive(w->done);
}
}

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

void UsbHostManager::poll() {}

void UsbHostManager::setInterfaceSelection(bool automatic, uint8_t interfaceNumber, uint8_t alternateSetting) {
  autoSelect_ = automatic;
  manualInterface_ = interfaceNumber;
  manualAlt_ = alternateSetting;
  g.selectionPending = true;
}

void UsbHostManager::requestStatusRefresh() {
  if (started_ && device_.attached && device_.statusInterfaceFound) g.statusPending = true;
}

const UsbPrinterInterfaceInfo *UsbHostManager::selectedInterface() const {
  return device_.printer.found ? &device_.printer : nullptr;
}

const UsbPrinterInterfaceInfo *UsbHostManager::interfaceAt(uint8_t index) const {
  return index < device_.printerInterfaceCount ? &device_.printerInterfaces[index] : nullptr;
}

bool UsbHostManager::bulkWrite(const uint8_t *data, size_t length, size_t &accepted,
                               uint32_t timeoutMs, String &error) {
  accepted = 0;
  if (!data || !length) {
    error = "empty USB transfer";
    return false;
  }
  if (!g.deviceOpen || !g.interfaceClaimed || !device_.printer.usableForRawPrint()) {
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
  t->callback = transferCallback;
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

void UsbHostManager::onEnumerated(const UsbDeviceInfo &info) {
  device_ = info;
  error_.clear();
  state_ = info.printer.found ? PRINTER_READY : DEVICE_ATTACHED;
}

void UsbHostManager::onDetached() {
  device_ = UsbDeviceInfo{};
  error_.clear();
  state_ = started_ ? RUNNING : STOPPED;
}

void UsbHostManager::onEnumerationError(const String &error) {
  error_ = error;
  if (started_) state_ = RUNNING;
}
