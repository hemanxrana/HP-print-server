#include "usb_host_manager.h"

#include <cstring>
#include "esp_intr_alloc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/usb_host.h"
#include "usb/usb_helpers.h"
#include "usb/usb_types_ch9.h"

namespace {

static constexpr uint8_t USB_CLASS_PRINTER = 0x07;
static constexpr uint8_t USB_SUBCLASS_PRINTER = 0x01;
static constexpr uint8_t USB_ENDPOINT_XFER_BULK = 0x02;
static constexpr uint8_t USB_ENDPOINT_DIR_IN = 0x80;

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
  volatile usb_device_handle_t gone_device = nullptr;
  TaskHandle_t client_task = nullptr;
};

UsbHostRuntime g_usb;
UsbHostManager *g_manager = nullptr;

#if defined(CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK)
static bool usbEnumFilter(const usb_device_desc_t *, uint8_t *) {
  // Arduino-ESP32 3.3.11 / ESP-IDF 5.5.5 can stall enumeration when this
  // feature is enabled and the callback is NULL. Allow all devices here;
  // printer selection is performed from the configuration descriptors.
  return true;
}
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
  const size_t totalLength = cfgDesc->wTotalLength;
  const usb_standard_desc_t *desc = usb_parse_next_descriptor(
      (const usb_standard_desc_t *)cfgDesc, totalLength, &offset);

  UsbPrinterInterfaceInfo *currentPrinter = nullptr;

  while (desc) {
    if (desc->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
      const usb_intf_desc_t *intf = (const usb_intf_desc_t *)desc;
      currentPrinter = nullptr;

      if (intf->bInterfaceClass == USB_CLASS_PRINTER &&
          intf->bInterfaceSubClass == USB_SUBCLASS_PRINTER &&
          intf->bInterfaceProtocol <= 0x04) {
        if (!out.printer.found) {
          out.printer.found = true;
          out.printer.interfaceNumber = intf->bInterfaceNumber;
          out.printer.alternateSetting = intf->bAlternateSetting;
          out.printer.subclass = intf->bInterfaceSubClass;
          out.printer.protocol = intf->bInterfaceProtocol;
        }
        if (intf->bInterfaceNumber == out.printer.interfaceNumber &&
            intf->bAlternateSetting == out.printer.alternateSetting) {
          currentPrinter = &out.printer;
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
        if ((ep->bEndpointAddress & USB_ENDPOINT_DIR_IN) != 0) {
          if (!currentPrinter->bulkIn.valid()) currentPrinter->bulkIn = info;
        } else {
          if (!currentPrinter->bulkOut.valid()) currentPrinter->bulkOut = info;
        }
      }
    }

    desc = usb_parse_next_descriptor((const usb_standard_desc_t *)desc, totalLength, &offset);
  }

  if (!out.printer.found) {
    error = "USB device has no standard Printer Class interface";
    closeCurrentDevice();
    return false;
  }

  // USB Printer Class requires a Bulk OUT endpoint; Bulk IN is optional.
  if (!out.printer.bulkOut.valid()) {
    error = "Printer Class interface has no Bulk OUT endpoint";
    closeCurrentDevice();
    return false;
  }

  return true;
}

static void clientEventCallback(const usb_host_client_event_msg_t *event, void *) {
  if (!event) return;

  switch (event->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
      // The callback never opens devices or performs descriptor work. This is
      // deliberate: the SDK requires the callback to remain short and lets
      // the client task perform the real state-machine work.
      if (!g_usb.device_open && !g_usb.new_device_pending) {
        g_usb.new_device_address = event->new_dev.address;
        g_usb.new_device_pending = true;
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
    }

    if (g_usb.new_device_pending && !g_usb.device_open) {
      const uint8_t address = g_usb.new_device_address;
      g_usb.new_device_pending = false;
      if (g_manager) g_manager->poll();

      UsbDeviceInfo info;
      String error;
      if (enumerateDevice(address, info, error)) {
        if (g_manager) g_manager->onEnumerated(info);
        Serial.println("[USB] Printer Class device enumerated");
        Serial.printf("[USB] VID: 0x%04X  PID: 0x%04X  address: %u\n", info.vid, info.pid, info.address);
      } else {
        if (g_manager) g_manager->onEnumerationError(error);
        Serial.printf("[USB] Ignored device at address %u: %s\n", address, error.c_str());
        // enumerateDevice() already closed non-printer/error devices. No retry
        // is scheduled, preventing a reconnect/error loop.
      }
    }
  }
}

static void hostTask(void *) {
  while (true) {
    uint32_t flags = 0;
    const esp_err_t err = usb_host_lib_handle_events(portMAX_DELAY, &flags);
    if (err != ESP_OK) {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
    // Host Library stays installed for the lifetime of the firmware. There is
    // no normal install/uninstall cycle, so reconnects do not recreate tasks.
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

  if (xTaskCreate(hostTask, "usb_host_lib", 4096, nullptr, 2, nullptr) != pdPASS) {
    state_ = ERROR;
    error_ = "Failed to create USB host library task";
    return false;
  }

  if (xTaskCreate(clientTask, "usb_printer_host", 6144, nullptr, 3, &g_usb.client_task) != pdPASS) {
    state_ = ERROR;
    error_ = "Failed to create USB printer client task";
    return false;
  }

  state_ = RUNNING;
  error_.clear();
  return true;
}

void UsbHostManager::poll() {
  // Intentionally empty. The USB client event loop has exactly one owner:
  // clientTask(). The Arduino loop must never call
  // usb_host_client_handle_events() as a second consumer.
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
  // A non-printer device is not a fatal USB-host error. Keep the host alive so
  // a later printer can be attached. Record the reason without retrying it.
  error_ = error;
  if (started_) state_ = RUNNING;
}
