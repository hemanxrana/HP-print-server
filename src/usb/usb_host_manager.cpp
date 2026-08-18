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

static constexpr uint8_t USB_CLASS_PRINTER = 0x07;
static constexpr uint8_t USB_SUBCLASS_PRINTER = 0x01;
static constexpr uint8_t USB_ENDPOINT_XFER_BULK = 0x02;
static constexpr uint8_t USB_ENDPOINT_DIR_IN = 0x80;
static constexpr int MAX_ALREADY_CONNECTED_DEVICES = 8;

struct BulkTransferContext {
  SemaphoreHandle_t done = nullptr;
  volatile bool callbackCalled = false;
};

struct UsbHostRuntime {
  usb_host_client_handle_t client = nullptr;
  usb_device_handle_t device = nullptr;
  uint8_t address = 0;
  bool device_open = false;
  uint8_t claimed_interface = 0;
  bool interface_claimed = false;
  volatile bool new_device_pending = false;
  volatile bool device_gone_pending = false;
  volatile uint8_t new_device_address = 0;
  volatile bool deferred_device_pending = false;
  volatile uint8_t deferred_device_address = 0;
  volatile usb_device_handle_t gone_device = nullptr;
  TaskHandle_t client_task = nullptr;
  TaskHandle_t host_task = nullptr;
};

UsbHostRuntime g_usb;
UsbHostManager *g_manager = nullptr;

#if defined(CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK)
static bool usbEnumFilter(const usb_device_desc_t *, uint8_t *) { return true; }
#endif

static String usbStringToArduino(const usb_str_desc_t *desc) {
  if (!desc || desc->bLength < 2) return String();
  const size_t chars = (desc->bLength - 2) / 2;
  String out;
  out.reserve(chars);
  for (size_t i = 0; i < chars; ++i) {
    const uint16_t c = desc->wData[i];
    if (c == 0) break;
    out += static_cast<char>(c <= 0x7F ? c : '?');
  }
  return out;
}

static bool isBulk(const usb_ep_desc_t *ep) {
  return ep && ((ep->bmAttributes & 0x03) == USB_ENDPOINT_XFER_BULK);
}

static void resetRuntimeDeviceState() {
  g_usb.device = nullptr;
  g_usb.address = 0;
  g_usb.device_open = false;
  g_usb.interface_claimed = false;
}

static void closeCurrentDevice() {
  if (!g_usb.device_open || !g_usb.device) {
    resetRuntimeDeviceState();
    return;
  }

  if (g_usb.interface_claimed) {
    usb_host_interface_release(g_usb.client, g_usb.device, g_usb.claimed_interface);
    g_usb.interface_claimed = false;
  }

  usb_host_device_close(g_usb.client, g_usb.device);
  resetRuntimeDeviceState();
}

static int scoreInterface(const UsbPrinterInterfaceInfo &p) {
  if (!p.usableForRawPrint()) return -1000;
  int score = 0;
  // Protocol 0x02 is the standard bidirectional USB Printer Class protocol
  // and is the best match for a raw/PCL-style backend.
  if (p.protocol == 0x02) score += 100;
  else if (p.protocol == 0x04) score += 80; // IPP-over-USB candidate, not raw.
  else if (p.protocol == 0x01 || p.protocol == 0x03) score += 60;
  else if (p.protocol == 0xFF) score += 20;
  if (p.bulkIn.valid()) score += 10;
  if (p.alternateSetting == 0) score += 2;
  return score;
}

static int findSelectedIndex(const UsbDeviceInfo &info, bool automatic, uint8_t manualInterface, uint8_t manualAlt) {
  int best = -1;
  int bestScore = -1001;

  for (uint8_t i = 0; i < info.printerInterfaceCount; ++i) {
    const UsbPrinterInterfaceInfo &p = info.printerInterfaces[i];
    if (!p.usableForRawPrint()) continue;

    if (!automatic) {
      if (p.interfaceNumber == manualInterface && p.alternateSetting == manualAlt) return i;
      continue;
    }

    const int score = scoreInterface(p);
    if (score > bestScore) {
      bestScore = score;
      best = i;
    }
  }
  return best;
}

static bool claimSelectedInterface(UsbDeviceInfo &out, String &error) {
  if (!g_manager) {
    error = "USB host manager unavailable";
    return false;
  }

  const int index = findSelectedIndex(out,
                                      g_manager->automaticInterfaceSelection(),
                                      g_manager->manualInterfaceNumber(),
                                      g_manager->manualAlternateSetting());
  if (index < 0) {
    error = g_manager->automaticInterfaceSelection()
                ? "No usable USB Printer Class Bulk OUT interface"
                : "Configured USB printer interface was not found";
    return false;
  }

  UsbPrinterInterfaceInfo &selected = out.printerInterfaces[index];
  const esp_err_t err = usb_host_interface_claim(g_usb.client, g_usb.device,
                                                  selected.interfaceNumber,
                                                  selected.alternateSetting);
  if (err != ESP_OK) {
    error = String("usb_host_interface_claim failed: ") + esp_err_to_name(err);
    return false;
  }

  g_usb.claimed_interface = selected.interfaceNumber;
  g_usb.interface_claimed = true;
  out.printer = selected;
  return true;
}

static bool enumerateDevice(uint8_t address, UsbDeviceInfo &out, String &error) {
  usb_device_handle_t dev = nullptr;
  const usb_device_desc_t *devDesc = nullptr;
  const usb_config_desc_t *cfgDesc = nullptr;

  esp_err_t err = usb_host_device_open(g_usb.client, address, &dev);
  if (err != ESP_OK) {
    error = String("usb_host_device_open failed: ") + esp_err_to_name(err);
    return false;
  }

  g_usb.device = dev;
  g_usb.address = address;
  g_usb.device_open = true;

  err = usb_host_get_device_descriptor(dev, &devDesc);
  if (err != ESP_OK || !devDesc) {
    error = String("device descriptor read failed: ") + esp_err_to_name(err);
    closeCurrentDevice();
    return false;
  }

  out = UsbDeviceInfo{};
  out.attached = true;
  out.address = address;
  out.vid = devDesc->idVendor;
  out.pid = devDesc->idProduct;

  usb_device_info_t devInfo{};
  if (usb_host_device_info(dev, &devInfo) == ESP_OK) {
    out.manufacturer = usbStringToArduino(devInfo.str_desc_manufacturer);
    out.product = usbStringToArduino(devInfo.str_desc_product);
    out.serial = usbStringToArduino(devInfo.str_desc_serial_num);
  }

  err = usb_host_get_active_config_descriptor(dev, &cfgDesc);
  if (err != ESP_OK || !cfgDesc) {
    error = String("configuration descriptor read failed: ") + esp_err_to_name(err);
    closeCurrentDevice();
    return false;
  }
  out.configurationValue = cfgDesc->bConfigurationValue;

  int offset = 0;
  const uint16_t totalLength = cfgDesc->wTotalLength;
  const usb_standard_desc_t *desc = usb_parse_next_descriptor(
      (const usb_standard_desc_t *)cfgDesc, totalLength, &offset);
  UsbPrinterInterfaceInfo *currentPrinter = nullptr;

  while (desc) {
    if (desc->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
      const usb_intf_desc_t *intf = (const usb_intf_desc_t *)desc;
      currentPrinter = nullptr;

      if (intf->bInterfaceClass == USB_CLASS_PRINTER &&
          intf->bInterfaceSubClass == USB_SUBCLASS_PRINTER &&
          ((intf->bInterfaceProtocol >= 0x01 && intf->bInterfaceProtocol <= 0x04) ||
           intf->bInterfaceProtocol == 0xFF)) {
        if (out.printerInterfaceCount < UsbDeviceInfo::MAX_PRINTER_INTERFACES) {
          UsbPrinterInterfaceInfo &p = out.printerInterfaces[out.printerInterfaceCount++];
          p.found = true;
          p.interfaceNumber = intf->bInterfaceNumber;
          p.alternateSetting = intf->bAlternateSetting;
          p.subclass = intf->bInterfaceSubClass;
          p.protocol = intf->bInterfaceProtocol;
          currentPrinter = &p;
        }
      }
    } else if (desc->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT && currentPrinter) {
      const usb_ep_desc_t *ep = (const usb_ep_desc_t *)desc;
      if (isBulk(ep)) {
        UsbEndpointInfo info;
        info.address = ep->bEndpointAddress;
        info.attributes = ep->bmAttributes;
        info.maxPacketSize = ep->wMaxPacketSize;
        info.interval = ep->bInterval;
        if (ep->bEndpointAddress & USB_ENDPOINT_DIR_IN) {
          if (!currentPrinter->bulkIn.valid()) currentPrinter->bulkIn = info;
        } else if (!currentPrinter->bulkOut.valid()) {
          currentPrinter->bulkOut = info;
        }
      }
    }

    desc = usb_parse_next_descriptor((const usb_standard_desc_t *)desc, totalLength, &offset);
  }

  if (out.printerInterfaceCount == 0) {
    error = "USB device has no USB Printer Class interface";
    closeCurrentDevice();
    return false;
  }

  if (!claimSelectedInterface(out, error)) {
    closeCurrentDevice();
    return false;
  }

  Serial.printf("[USB] %u printer-class interface candidates found\n", out.printerInterfaceCount);
  for (uint8_t i = 0; i < out.printerInterfaceCount; ++i) {
    const UsbPrinterInterfaceInfo &p = out.printerInterfaces[i];
    Serial.printf("[USB] Candidate %u: IF=%u ALT=%u protocol=0x%02X OUT=0x%02X IN=0x%02X score=%d\n",
                  i, p.interfaceNumber, p.alternateSetting, p.protocol,
                  p.bulkOut.address, p.bulkIn.address, scoreInterface(p));
  }

  return true;
}

static void publishEnumerationResult(const UsbDeviceInfo &info) {
  if (!g_manager) return;
  g_manager->onEnumerated(info);
  Serial.printf("[USB] Printer: %s\n", info.product.length() ? info.product.c_str() : "USB Printer");
  Serial.printf("[USB] VID: 0x%04X PID: 0x%04X address=%u\n", info.vid, info.pid, info.address);
  Serial.printf("[USB] SELECTED IF=%u ALT=%u protocol=0x%02X OUT=0x%02X IN=0x%02X\n",
                info.printer.interfaceNumber, info.printer.alternateSetting,
                info.printer.protocol, info.printer.bulkOut.address,
                info.printer.bulkIn.address);
}

static void queueDeferredDeviceIfNeeded() {
  if (!g_usb.device_open && g_usb.deferred_device_pending && !g_usb.new_device_pending) {
    g_usb.new_device_address = g_usb.deferred_device_address;
    g_usb.new_device_pending = true;
    g_usb.deferred_device_pending = false;
  }
}

static void scanAlreadyConnectedDevices() {
  uint8_t addresses[MAX_ALREADY_CONNECTED_DEVICES] = {};
  int count = 0;
  const esp_err_t err = usb_host_device_addr_list_fill(MAX_ALREADY_CONNECTED_DEVICES, addresses, &count);
  if (err != ESP_OK || count <= 0) return;

  for (int i = 0; i < count && !g_usb.device_open; ++i) {
    UsbDeviceInfo info;
    String error;
    if (enumerateDevice(addresses[i], info, error)) {
      publishEnumerationResult(info);
      return;
    }
    Serial.printf("[USB] Existing device at address %u ignored: %s\n", addresses[i], error.c_str());
  }
}

static void clientEventCallback(const usb_host_client_event_msg_t *event, void *) {
  if (!event) return;
  switch (event->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
      if (!g_usb.device_open && !g_usb.new_device_pending) {
        g_usb.new_device_address = event->new_dev.address;
        g_usb.new_device_pending = true;
      } else if (g_usb.device_open && !g_usb.deferred_device_pending) {
        g_usb.deferred_device_address = event->new_dev.address;
        g_usb.deferred_device_pending = true;
      }
      break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
      if (g_usb.device_open && event->dev_gone.dev_hdl == g_usb.device) {
        g_usb.gone_device = event->dev_gone.dev_hdl;
        g_usb.device_gone_pending = true;
      }
      break;
    default:
      break;
  }
}

static void clientTask(void *) {
  usb_host_client_config_t config{};
  config.is_synchronous = false;
  config.max_num_event_msg = 8;
  config.async.client_event_callback = clientEventCallback;
  config.async.callback_arg = nullptr;

  const esp_err_t regErr = usb_host_client_register(&config, &g_usb.client);
  if (regErr != ESP_OK) {
    if (g_manager) g_manager->onEnumerationError(String("usb_host_client_register failed: ") + esp_err_to_name(regErr));
    vTaskDelete(nullptr);
    return;
  }

  scanAlreadyConnectedDevices();

  while (true) {
    const esp_err_t eventErr = usb_host_client_handle_events(g_usb.client, pdMS_TO_TICKS(100));
    if (eventErr != ESP_OK && eventErr != ESP_ERR_TIMEOUT) {
      if (g_manager) g_manager->onEnumerationError(String("USB client event handling failed: ") + esp_err_to_name(eventErr));
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    if (g_usb.device_gone_pending) {
      g_usb.device_gone_pending = false;
      closeCurrentDevice();
      if (g_manager) g_manager->onDetached();
      queueDeferredDeviceIfNeeded();
    }

    if (g_usb.new_device_pending && !g_usb.device_open) {
      const uint8_t address = g_usb.new_device_address;
      g_usb.new_device_pending = false;
      g_manager->onEnumerationError("");
      g_manager->onEnumerated(UsbDeviceInfo{});
      g_manager->onEnumerationError("enumerating-usb-device");
      UsbDeviceInfo info;
      String error;
      if (enumerateDevice(address, info, error)) {
        publishEnumerationResult(info);
      } else {
        if (g_manager) g_manager->onEnumerationError(error);
        Serial.printf("[USB] Ignored device at address %u: %s\n", address, error.c_str());
        queueDeferredDeviceIfNeeded();
      }
    }
  }
}

static void hostTask(void *) {
  while (true) {
    uint32_t flags = 0;
    const esp_err_t err = usb_host_lib_handle_events(portMAX_DELAY, &flags);
    if (err != ESP_OK) vTaskDelay(pdMS_TO_TICKS(10));
  }
}

static void bulkTransferCallback(usb_transfer_t *transfer) {
  if (!transfer) return;
  BulkTransferContext *ctx = static_cast<BulkTransferContext *>(transfer->context);
  if (ctx) {
    ctx->callbackCalled = true;
    BaseType_t higherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(ctx->done, &higherPriorityTaskWoken);
    if (higherPriorityTaskWoken == pdTRUE) portYIELD_FROM_ISR();
  }
}

} // namespace

bool UsbHostManager::begin() {
  if (started_) return state_ != ERROR;
  started_ = true;
  g_manager = this;

  usb_host_config_t hostConfig{};
  hostConfig.skip_phy_setup = false;
  hostConfig.intr_flags = ESP_INTR_FLAG_LEVEL1;
#if defined(CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK)
  hostConfig.enum_filter_cb = usbEnumFilter;
#endif

  const esp_err_t err = usb_host_install(&hostConfig);
  if (err != ESP_OK) {
    state_ = ERROR;
    error_ = String("usb_host_install failed: ") + esp_err_to_name(err);
    return false;
  }

  if (xTaskCreate(hostTask, "usb_host_lib", 4096, nullptr, 2, &g_usb.host_task) != pdPASS) {
    usb_host_uninstall();
    state_ = ERROR;
    error_ = "Failed to create USB host library task";
    return false;
  }
  if (xTaskCreate(clientTask, "usb_printer_host", 8192, nullptr, 3, &g_usb.client_task) != pdPASS) {
    vTaskDelete(g_usb.host_task);
    g_usb.host_task = nullptr;
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

  if (!started_ || !g_usb.device_open) return;

  // Re-enumeration is deliberately serialized through the USB client task in
  // a future reconnect event. We do not release/claim an active interface from
  // the HTTP task while a transfer may be in flight.
  error_ = "selection saved; reconnect printer to apply";
}

const UsbPrinterInterfaceInfo *UsbHostManager::selectedInterface() const {
  return device_.printer.found ? &device_.printer : nullptr;
}

const UsbPrinterInterfaceInfo *UsbHostManager::interfaceAt(uint8_t index) const {
  if (index >= device_.printerInterfaceCount) return nullptr;
  return &device_.printerInterfaces[index];
}

bool UsbHostManager::bulkWrite(const uint8_t *data, size_t length, size_t &accepted, uint32_t timeoutMs, String &error) {
  accepted = 0;
  if (!data || length == 0) {
    error = "empty USB transfer";
    return false;
  }
  if (!g_usb.device_open || !g_usb.interface_claimed || !device_.printer.usableForRawPrint()) {
    error = "USB printer interface is not claimed/ready";
    return false;
  }
  if (length > 16384) {
    error = "USB transfer chunk exceeds 16 KiB";
    return false;
  }

  usb_transfer_t *transfer = nullptr;
  esp_err_t err = usb_host_transfer_alloc(length, 0, &transfer);
  if (err != ESP_OK || !transfer) {
    error = String("usb_host_transfer_alloc failed: ") + esp_err_to_name(err);
    return false;
  }

  BulkTransferContext ctx;
  ctx.done = xSemaphoreCreateBinary();
  if (!ctx.done) {
    usb_host_transfer_free(transfer);
    error = "unable to allocate transfer completion semaphore";
    return false;
  }

  memcpy(transfer->data_buffer, data, length);
  transfer->data_buffer_size = length;
  transfer->num_bytes = length;
  transfer->device_handle = g_usb.device;
  transfer->bEndpointAddress = device_.printer.bulkOut.address;
  transfer->callback = bulkTransferCallback;
  transfer->context = &ctx;
  transfer->timeout_ms = timeoutMs;

  err = usb_host_transfer_submit(transfer);
  if (err != ESP_OK) {
    vSemaphoreDelete(ctx.done);
    usb_host_transfer_free(transfer);
    error = String("usb_host_transfer_submit failed: ") + esp_err_to_name(err);
    return false;
  }

  const TickType_t waitTicks = pdMS_TO_TICKS(timeoutMs + 250);
  if (xSemaphoreTake(ctx.done, waitTicks) != pdTRUE) {
    usb_host_transfer_cancel(transfer);
    vSemaphoreDelete(ctx.done);
    usb_host_transfer_free(transfer);
    error = "USB bulk transfer timed out";
    return false;
  }

  accepted = transfer->actual_num_bytes;
  const usb_transfer_status_t status = transfer->status;
  vSemaphoreDelete(ctx.done);
  usb_host_transfer_free(transfer);

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
