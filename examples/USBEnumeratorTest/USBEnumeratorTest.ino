/*
 * HP-print-server - ESP32-S3 USB Enumerator Test
 *
 * Arduino IDE diagnostic firmware.
 *
 * Objective:
 *   Detect a USB device, open it through the ESP32-S3 USB Host client,
 *   dump its cached device/configuration descriptors, and identify printer
 *   interfaces and their endpoints.
 *
 * This sketch intentionally DOES NOT claim an interface or send print data.
 * It is safe for collecting the descriptors needed before implementing the
 * printer transfer backend.
 */

#include <Arduino.h>

#if __has_include("usb/usb_host.h") && __has_include("usb/usb_helpers.h")
#include "usb/usb_host.h"
#include "usb/usb_helpers.h"
#define HP_USB_HOST_API_AVAILABLE 1
#else
#define HP_USB_HOST_API_AVAILABLE 0
#endif

#if HP_USB_HOST_API_AVAILABLE

static usb_host_client_handle_t g_client = nullptr;

static void printHex16(uint16_t value) {
  Serial.printf("%04X", value);
}

static const char *speedName(usb_speed_t speed) {
  switch (speed) {
    case USB_SPEED_LOW:  return "LOW";
    case USB_SPEED_FULL: return "FULL";
    case USB_SPEED_HIGH: return "HIGH";
    default:             return "UNKNOWN";
  }
}

static const char *transferTypeName(uint8_t attributes) {
  switch (attributes & 0x03U) {
    case 0: return "CONTROL";
    case 1: return "ISOCHRONOUS";
    case 2: return "BULK";
    case 3: return "INTERRUPT";
    default: return "UNKNOWN";
  }
}

static void dumpDeviceDescriptor(usb_device_handle_t device) {
  const usb_device_desc_t *desc = nullptr;
  const esp_err_t err = usb_host_get_device_descriptor(device, &desc);
  if (err != ESP_OK || desc == nullptr) {
    Serial.printf("[ENUM] Device descriptor FAILED: %s\n", esp_err_to_name(err));
    return;
  }

  Serial.println("[ENUM] Device Descriptor");
  Serial.printf("  USB version : %u.%02u\n", desc->bcdUSB >> 8, desc->bcdUSB & 0xFF);
  Serial.print("  VID         : 0x"); printHex16(desc->idVendor); Serial.println();
  Serial.print("  PID         : 0x"); printHex16(desc->idProduct); Serial.println();
  Serial.printf("  Device class: 0x%02X\n", desc->bDeviceClass);
  Serial.printf("  Subclass    : 0x%02X\n", desc->bDeviceSubClass);
  Serial.printf("  Protocol    : 0x%02X\n", desc->bDeviceProtocol);
  Serial.printf("  EP0 MPS     : %u\n", desc->bMaxPacketSize0);
  Serial.printf("  Configs     : %u\n", desc->bNumConfigurations);
  Serial.printf("  Mfr index   : %u\n", desc->iManufacturer);
  Serial.printf("  Product idx : %u\n", desc->iProduct);
  Serial.printf("  Serial idx  : %u\n", desc->iSerialNumber);
}

static void dumpConfigurationDescriptor(usb_device_handle_t device) {
  const usb_config_desc_t *config = nullptr;
  const esp_err_t err = usb_host_get_active_config_descriptor(device, &config);
  if (err != ESP_OK || config == nullptr) {
    Serial.printf("[ENUM] Configuration descriptor FAILED: %s\n", esp_err_to_name(err));
    return;
  }

  Serial.println("[ENUM] Configuration Descriptor");
  Serial.printf("  Value       : %u\n", config->bConfigurationValue);
  Serial.printf("  Total length: %u\n", config->wTotalLength);
  Serial.printf("  Interfaces  : %u\n", config->bNumInterfaces);
  Serial.printf("  Attributes  : 0x%02X\n", config->bmAttributes);
  Serial.printf("  Max power   : %u mA\n", static_cast<unsigned>(config->bMaxPower) * 2U);

  // usb_parse_next_descriptor() uses an offset into the full configuration
  // descriptor. Start at the configuration descriptor itself.
  int offset = 0;
  const usb_standard_desc_t *cur =
      reinterpret_cast<const usb_standard_desc_t *>(config);

  while (true) {
    cur = usb_parse_next_descriptor(cur, config->wTotalLength, &offset);
    if (cur == nullptr) break;

    if (cur->bDescriptorType == 0x04) { // Interface descriptor
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
    } else if (cur->bDescriptorType == 0x05) { // Endpoint descriptor
      const usb_ep_desc_t *ep = reinterpret_cast<const usb_ep_desc_t *>(cur);
      const bool in = (ep->bEndpointAddress & 0x80U) != 0;
      Serial.printf("  Endpoint 0x%02X  %s  %s  maxPacket=%u interval=%u\n",
                    ep->bEndpointAddress,
                    in ? "IN " : "OUT",
                    transferTypeName(ep->bmAttributes),
                    ep->wMaxPacketSize,
                    ep->bInterval);
    }
  }
}

static void inspectDevice(uint8_t address) {
  usb_device_handle_t device = nullptr;
  esp_err_t err = usb_host_device_open(g_client, address, &device);
  if (err != ESP_OK || device == nullptr) {
    Serial.printf("[ENUM] usb_host_device_open(%u) FAILED: %s\n",
                  address, esp_err_to_name(err));
    return;
  }

  usb_device_info_t info{};
  err = usb_host_device_info(device, &info);
  if (err == ESP_OK) {
    Serial.printf("[ENUM] Device address=%u speed=%s\n",
                  address, speedName(info.speed));
  } else {
    Serial.printf("[ENUM] usb_host_device_info FAILED: %s\n", esp_err_to_name(err));
  }

  dumpDeviceDescriptor(device);
  dumpConfigurationDescriptor(device);

  // We intentionally do not claim an interface in this test. Therefore the
  // device can be closed immediately after descriptor inspection.
  err = usb_host_device_close(g_client, device);
  if (err != ESP_OK) {
    Serial.printf("[ENUM] usb_host_device_close FAILED: %s\n", esp_err_to_name(err));
  }

  Serial.println("[ENUM] Inspection complete. No printer transfer was attempted.");
}

static void clientEventCallback(const usb_host_client_event_msg_t *eventMsg, void *arg) {
  if (eventMsg == nullptr) return;

  switch (eventMsg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
      Serial.printf("\n[USB] NEW DEVICE address=%u\n", eventMsg->new_dev.address);
      inspectDevice(eventMsg->new_dev.address);
      break;

    case USB_HOST_CLIENT_EVENT_DEV_GONE:
      Serial.println("[USB] DEVICE GONE (a device opened by this client was removed)");
      break;

    default:
      break;
  }
}

static void hostLibraryTask(void *arg) {
  while (true) {
    uint32_t flags = 0;
    const esp_err_t err = usb_host_lib_handle_events(portMAX_DELAY, &flags);
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
      Serial.printf("[USB] host library events FAILED: %s\n", esp_err_to_name(err));
    }
  }
}

static void clientTask(void *arg) {
  usb_host_client_config_t clientConfig{};
  clientConfig.is_synchronous = false;
  clientConfig.max_num_event_msg = 8;
  clientConfig.async.client_event_callback = clientEventCallback;
  clientConfig.async.callback_arg = nullptr;

  esp_err_t err = usb_host_client_register(&clientConfig, &g_client);
  if (err != ESP_OK) {
    Serial.printf("[USB] usb_host_client_register FAILED: %s\n", esp_err_to_name(err));
    vTaskDelete(nullptr);
    return;
  }

  Serial.println("[USB] USB Host client registered.");
  Serial.println("[USB] Checking for devices already connected...");

  uint8_t addresses[8] = {};
  int count = 0;
  err = usb_host_device_addr_list_fill(
      static_cast<int>(sizeof(addresses)), addresses, &count);

  if (err == ESP_OK) {
    for (int i = 0; i < count; ++i) {
      Serial.printf("[USB] Already-enumerated device address=%u\n", addresses[i]);
      inspectDevice(addresses[i]);
    }
  } else {
    Serial.printf("[USB] Existing-device scan FAILED: %s\n", esp_err_to_name(err));
  }

  Serial.println("[USB] Waiting for USB attach/detach events...");

  while (true) {
    err = usb_host_client_handle_events(g_client, portMAX_DELAY);
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
      Serial.printf("[USB] client events FAILED: %s\n", esp_err_to_name(err));
    }
  }
}

#endif // HP_USB_HOST_API_AVAILABLE

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("========================================");
  Serial.println(" ESP32-S3 USB ENUMERATOR TEST");
  Serial.println("========================================");
  Serial.printf("Arduino-ESP32: %s\n", ESP_ARDUINO_VERSION_STR);

#if HP_USB_HOST_API_AVAILABLE
  usb_host_config_t hostConfig{};
  hostConfig.skip_phy_setup = false;
  hostConfig.intr_flags = ESP_INTR_FLAG_LEVEL1;

  const esp_err_t err = usb_host_install(&hostConfig);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    Serial.printf("[USB] usb_host_install FAILED: %s\n", esp_err_to_name(err));
    return;
  }

  Serial.println("[USB] Host library installed.");
  Serial.println("[USB] Plug the printer into the ESP32-S3 USB Host/OTG port.");

  BaseType_t taskResult = xTaskCreatePinnedToCore(
      hostLibraryTask, "usb_host_lib", 4096, nullptr, 20, nullptr, 0);
  if (taskResult != pdPASS) {
    Serial.println("[USB] FAILED to create host library task.");
    return;
  }

  taskResult = xTaskCreatePinnedToCore(
      clientTask, "usb_host_client", 8192, nullptr, 19, nullptr, 0);
  if (taskResult != pdPASS) {
    Serial.println("[USB] FAILED to create USB client task.");
    return;
  }
#else
  Serial.println("[ERROR] usb/usb_host.h or usb/usb_helpers.h is not available.");
  Serial.println("[ERROR] Tell me the exact Arduino-ESP32 core version from Arduino IDE.");
#endif
}

void loop() {
  delay(1000);
}
