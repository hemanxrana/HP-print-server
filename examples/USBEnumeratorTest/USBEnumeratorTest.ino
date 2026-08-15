/*
 * HP-print-server - ESP32-S3 USB Enumerator Test
 *
 * Purpose:
 *   Enumerate the USB device connected to the ESP32-S3 OTG/Host port and
 *   print the information we need before implementing printer transfers.
 *
 * This sketch intentionally does NOT send printer data.
 *
 * IMPORTANT:
 *   This is an Arduino IDE diagnostic sketch. It uses the Arduino-ESP32 USB
 *   Host API exposed by the installed core. If your installed core exposes a
 *   different API, compile errors here tell us exactly which adapter layer
 *   needs to be adjusted; do not guess endpoint values.
 */

#include <Arduino.h>
#include "USB.h"
#include "USBHID.h"

#if __has_include("usb/usb_host.h")
#include "usb/usb_host.h"
#define HP_USB_HOST_API_AVAILABLE 1
#else
#define HP_USB_HOST_API_AVAILABLE 0
#endif

static void printHex16(uint16_t value) {
  if (value < 0x1000) Serial.print('0');
  if (value < 0x0100) Serial.print('0');
  if (value < 0x0010) Serial.print('0');
  Serial.print(value, HEX);
}

#if HP_USB_HOST_API_AVAILABLE

static const char *speedName(usb_speed_t speed) {
  switch (speed) {
    case USB_SPEED_LOW:  return "LOW";
    case USB_SPEED_FULL: return "FULL";
    case USB_SPEED_HIGH: return "HIGH";
    default:             return "UNKNOWN";
  }
}

static void dumpDeviceDescriptor(usb_device_handle_t device) {
  const usb_device_desc_t *desc = nullptr;
  esp_err_t err = usb_host_get_device_descriptor(device, &desc);
  if (err != ESP_OK || desc == nullptr) {
    Serial.printf("[ENUM] Device descriptor: FAILED (%s)\n", esp_err_to_name(err));
    return;
  }

  Serial.println("[ENUM] Device Descriptor");
  Serial.printf("  USB version : %u.%02u\n", desc->bcdUSB >> 8, desc->bcdUSB & 0xFF);
  Serial.print("  VID         : 0x"); printHex16(desc->idVendor); Serial.println();
  Serial.print("  PID         : 0x"); printHex16(desc->idProduct); Serial.println();
  Serial.printf("  Class       : 0x%02X\n", desc->bDeviceClass);
  Serial.printf("  Subclass    : 0x%02X\n", desc->bDeviceSubClass);
  Serial.printf("  Protocol    : 0x%02X\n", desc->bDeviceProtocol);
  Serial.printf("  Configs     : %u\n", desc->bNumConfigurations);
  Serial.printf("  Mfr index   : %u\n", desc->iManufacturer);
  Serial.printf("  Product idx : %u\n", desc->iProduct);
  Serial.printf("  Serial idx  : %u\n", desc->iSerialNumber);
}

static void dumpConfigurationDescriptor(usb_device_handle_t device) {
  const usb_config_desc_t *config = nullptr;
  esp_err_t err = usb_host_get_active_config_descriptor(device, &config);
  if (err != ESP_OK || config == nullptr) {
    Serial.printf("[ENUM] Configuration descriptor: FAILED (%s)\n", esp_err_to_name(err));
    return;
  }

  Serial.println("[ENUM] Configuration Descriptor");
  Serial.printf("  Value       : %u\n", config->bConfigurationValue);
  Serial.printf("  Interfaces  : %u\n", config->bNumInterfaces);
  Serial.printf("  Attributes  : 0x%02X\n", config->bmAttributes);
  Serial.printf("  Max power   : %u mA\n", static_cast<unsigned>(config->bMaxPower) * 2U);

  const usb_standard_desc_t *cur = nullptr;
  while ((cur = usb_parse_next_descriptor(cur, config)) != nullptr) {
    if (cur->bDescriptorType == USB_WIRE_DESC_ITF) {
      const usb_intf_desc_t *itf = reinterpret_cast<const usb_intf_desc_t *>(cur);
      Serial.printf("\n[ENUM] Interface %u alt=%u class=0x%02X subclass=0x%02X protocol=0x%02X endpoints=%u\n",
                    itf->bInterfaceNumber,
                    itf->bAlternateSetting,
                    itf->bInterfaceClass,
                    itf->bInterfaceSubClass,
                    itf->bInterfaceProtocol,
                    itf->bNumEndpoints);

      if (itf->bInterfaceClass == 0x07) {
        Serial.println("  >>> USB Printer Class interface detected <<<");
      }
    } else if (cur->bDescriptorType == USB_WIRE_DESC_EP) {
      const usb_ep_desc_t *ep = reinterpret_cast<const usb_ep_desc_t *>(cur);
      const uint8_t transferType = ep->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK;
      const bool in = (ep->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK) != 0;

      const char *typeName = "OTHER";
      if (transferType == USB_BM_ATTRIBUTES_XFER_BULK) typeName = "BULK";
      else if (transferType == USB_BM_ATTRIBUTES_XFER_INT) typeName = "INTERRUPT";
      else if (transferType == USB_BM_ATTRIBUTES_XFER_ISOC) typeName = "ISOCHRONOUS";
      else if (transferType == USB_BM_ATTRIBUTES_XFER_CONTROL) typeName = "CONTROL";

      Serial.printf("  Endpoint 0x%02X  %s  %s  maxPacket=%u interval=%u\n",
                    ep->bEndpointAddress,
                    in ? "IN " : "OUT",
                    typeName,
                    ep->wMaxPacketSize,
                    ep->bInterval);
    }
  }
}

static void inspectDevice(uint8_t address) {
  usb_device_handle_t device = nullptr;
  esp_err_t err = usb_host_device_open(nullptr, address, &device);
  if (err != ESP_OK || device == nullptr) {
    Serial.printf("[ENUM] usb_host_device_open(%u) failed: %s\n", address, esp_err_to_name(err));
    return;
  }

  usb_speed_t speed = USB_SPEED_FULL;
  if (usb_host_device_info(device, nullptr) == ESP_OK) {
    usb_device_info_t info{};
    if (usb_host_device_info(device, &info) == ESP_OK) {
      speed = info.speed;
      Serial.printf("[ENUM] Device address=%u speed=%s\n", address, speedName(info.speed));
    }
  }

  dumpDeviceDescriptor(device);
  dumpConfigurationDescriptor(device);

  err = usb_host_device_close(nullptr, device);
  if (err != ESP_OK) {
    Serial.printf("[ENUM] usb_host_device_close failed: %s\n", esp_err_to_name(err));
  }
}

static void usbHostTask(void *arg) {
  usb_host_config_t hostConfig{};
  hostConfig.skip_phy_setup = false;
  hostConfig.intr_flags = ESP_INTR_FLAG_LEVEL1;

  esp_err_t err = usb_host_install(&hostConfig);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    Serial.printf("[USB] usb_host_install failed: %s\n", esp_err_to_name(err));
    vTaskDelete(nullptr);
    return;
  }

  Serial.println("[USB] Host installed. Plug the printer into the ESP32-S3 USB Host/OTG port.");

  usb_host_client_config_t clientConfig{};
  clientConfig.is_synchronous = true;
  clientConfig.max_num_event_msg = 8;
  clientConfig.async.client_event_callback = [](const usb_host_client_event_msg_t *eventMsg, void *arg) {
    if (!eventMsg) return;

    if (eventMsg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
      Serial.printf("\n[USB] NEW DEVICE address=%u\n", eventMsg->new_dev.address);
      inspectDevice(eventMsg->new_dev.address);
    } else if (eventMsg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
      Serial.printf("[USB] DEVICE GONE address=%u\n", eventMsg->dev_gone.dev_hdl);
    }
  };
  clientConfig.async.callback_arg = nullptr;

  usb_host_client_handle_t client = nullptr;
  err = usb_host_client_register(&clientConfig, &client);
  if (err != ESP_OK) {
    Serial.printf("[USB] usb_host_client_register failed: %s\n", esp_err_to_name(err));
    vTaskDelete(nullptr);
    return;
  }

  Serial.println("[USB] Client registered; waiting for enumeration events.");

  while (true) {
    uint32_t flags = 0;
    err = usb_host_lib_handle_events(portMAX_DELAY, &flags);
    if (err != ESP_OK) {
      Serial.printf("[USB] host_lib_handle_events: %s\n", esp_err_to_name(err));
    }
    usb_host_client_handle_events(client, portMAX_DELAY);
  }
}

#endif

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("========================================");
  Serial.println(" ESP32-S3 USB ENUMERATOR TEST");
  Serial.println("========================================");
  Serial.printf("Arduino-ESP32: %s\n", ESP_ARDUINO_VERSION_STR);

#if HP_USB_HOST_API_AVAILABLE
  xTaskCreatePinnedToCore(usbHostTask, "usb_host_test", 8192, nullptr, 5, nullptr, 0);
#else
  Serial.println("[ERROR] usb/usb_host.h is not available in this Arduino-ESP32 installation.");
  Serial.println("[ERROR] Tell me the exact Arduino-ESP32 core version shown by Arduino IDE.");
#endif
}

void loop() {
  delay(1000);
}
