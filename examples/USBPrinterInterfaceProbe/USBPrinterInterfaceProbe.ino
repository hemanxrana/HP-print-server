/*
 * HP-print-server - USB Printer Interface Probe
 *
 * Diagnostic-only firmware.
 *
 * Goal:
 *   Enumerate the connected USB printer and rank its printer-class
 *   interfaces without sending print data.
 *
 * This does NOT claim an interface or submit Bulk transfers.
 * It is intentionally safe: it only reads descriptors and identifies
 * likely printer interfaces.
 */

#include <Arduino.h>
#include "USB.h"

#if __has_include("usb/usb_host.h")
#include "usb/usb_host.h"
#define HP_USB_HOST_API_AVAILABLE 1
#else
#define HP_USB_HOST_API_AVAILABLE 0
#endif

#if HP_USB_HOST_API_AVAILABLE

struct PrinterCandidate {
  uint8_t interfaceNumber = 0;
  uint8_t alternateSetting = 0;
  uint8_t protocol = 0;
  uint8_t bulkOut = 0;
  uint8_t bulkIn = 0;
  uint16_t bulkOutMps = 0;
  uint16_t bulkInMps = 0;
  bool hasBulkOut = false;
  bool hasBulkIn = false;
};

static PrinterCandidate candidates[16];
static size_t candidateCount = 0;

static void resetCandidates() {
  candidateCount = 0;
  memset(candidates, 0, sizeof(candidates));
}

static PrinterCandidate *candidateFor(uint8_t interfaceNumber, uint8_t alt,
                                      uint8_t protocol) {
  for (size_t i = 0; i < candidateCount; ++i) {
    if (candidates[i].interfaceNumber == interfaceNumber &&
        candidates[i].alternateSetting == alt &&
        candidates[i].protocol == protocol) {
      return &candidates[i];
    }
  }

  if (candidateCount >= 16) return nullptr;

  PrinterCandidate &c = candidates[candidateCount++];
  c.interfaceNumber = interfaceNumber;
  c.alternateSetting = alt;
  c.protocol = protocol;
  return &c;
}

static int scoreCandidate(const PrinterCandidate &c) {
  if (!c.hasBulkOut) return -1000;

  int score = 0;

  // Standard bidirectional USB Printer Class protocol is preferred.
  if (c.protocol == 0x02) score += 100;
  else if (c.protocol == 0x04) score += 50;

  // A readable IN endpoint is useful for printer status.
  if (c.hasBulkIn) score += 20;

  // 64-byte bulk endpoints are normal for this Full-Speed printer.
  if (c.bulkOutMps == 64) score += 10;
  if (c.bulkInMps == 64) score += 5;

  return score;
}

static void printCandidate(const PrinterCandidate &c, size_t index) {
  Serial.printf("\nCandidate #%u\n", static_cast<unsigned>(index + 1));
  Serial.printf("  Interface      : %u\n", c.interfaceNumber);
  Serial.printf("  Alternate      : %u\n", c.alternateSetting);
  Serial.printf("  Protocol       : 0x%02X\n", c.protocol);

  if (c.hasBulkOut) {
    Serial.printf("  Bulk OUT       : 0x%02X (%u bytes)\n", c.bulkOut, c.bulkOutMps);
  } else {
    Serial.println("  Bulk OUT       : NONE");
  }

  if (c.hasBulkIn) {
    Serial.printf("  Bulk IN        : 0x%02X (%u bytes)\n", c.bulkIn, c.bulkInMps);
  } else {
    Serial.println("  Bulk IN        : NONE");
  }

  Serial.printf("  Score          : %d\n", scoreCandidate(c));
}

static void probeConfiguration(usb_device_handle_t device) {
  const usb_config_desc_t *config = nullptr;
  esp_err_t err = usb_host_get_active_config_descriptor(device, &config);
  if (err != ESP_OK || config == nullptr) {
    Serial.printf("[PROBE] Configuration descriptor failed: %s\n", esp_err_to_name(err));
    return;
  }

  resetCandidates();

  const usb_standard_desc_t *cur = nullptr;
  uint8_t currentInterface = 0xFF;
  uint8_t currentAlt = 0xFF;
  uint8_t currentProtocol = 0;
  PrinterCandidate *currentCandidate = nullptr;

  while ((cur = usb_parse_next_descriptor(cur, config)) != nullptr) {
    if (cur->bDescriptorType == USB_WIRE_DESC_ITF) {
      const usb_intf_desc_t *itf = reinterpret_cast<const usb_intf_desc_t *>(cur);

      currentInterface = itf->bInterfaceNumber;
      currentAlt = itf->bAlternateSetting;
      currentProtocol = itf->bInterfaceProtocol;
      currentCandidate = nullptr;

      if (itf->bInterfaceClass == 0x07 && itf->bInterfaceSubClass == 0x01) {
        currentCandidate = candidateFor(currentInterface, currentAlt, currentProtocol);
        if (currentCandidate) {
          Serial.printf("[PROBE] Printer Class candidate: interface=%u alt=%u protocol=0x%02X\n",
                        currentInterface, currentAlt, currentProtocol);
        }
      }
    } else if (cur->bDescriptorType == USB_WIRE_DESC_EP && currentCandidate) {
      const usb_ep_desc_t *ep = reinterpret_cast<const usb_ep_desc_t *>(cur);
      const uint8_t type = ep->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK;
      const bool isBulk = type == USB_BM_ATTRIBUTES_XFER_BULK;
      const bool isIn = (ep->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK) != 0;

      if (!isBulk) continue;

      if (isIn) {
        currentCandidate->hasBulkIn = true;
        currentCandidate->bulkIn = ep->bEndpointAddress;
        currentCandidate->bulkInMps = ep->wMaxPacketSize;
      } else {
        currentCandidate->hasBulkOut = true;
        currentCandidate->bulkOut = ep->bEndpointAddress;
        currentCandidate->bulkOutMps = ep->wMaxPacketSize;
      }
    }
  }

  Serial.println();
  Serial.println("========== PRINTER INTERFACE PROBE ==========");
  Serial.printf("Printer-class candidates: %u\n", static_cast<unsigned>(candidateCount));

  int bestScore = -10000;
  int bestIndex = -1;

  for (size_t i = 0; i < candidateCount; ++i) {
    printCandidate(candidates[i], i);
    const int score = scoreCandidate(candidates[i]);
    if (score > bestScore) {
      bestScore = score;
      bestIndex = static_cast<int>(i);
    }
  }

  if (bestIndex >= 0 && bestScore > 0) {
    const PrinterCandidate &best = candidates[bestIndex];
    Serial.println();
    Serial.println("========== BEST CANDIDATE ==========");
    Serial.printf("Interface      : %u\n", best.interfaceNumber);
    Serial.printf("Alternate      : %u\n", best.alternateSetting);
    Serial.printf("Protocol       : 0x%02X\n", best.protocol);
    Serial.printf("Bulk OUT       : 0x%02X\n", best.bulkOut);
    Serial.printf("Bulk IN        : 0x%02X\n", best.bulkIn);
    Serial.printf("Score          : %d\n", bestScore);
    Serial.println();
    Serial.println("This is a diagnostic selection only.");
    Serial.println("No interface was claimed and no print data was sent.");
  } else {
    Serial.println("[PROBE] No usable printer candidate found.");
  }

  Serial.println("=============================================");
}

static void inspectDevice(uint8_t address, usb_host_client_handle_t client) {
  usb_device_handle_t device = nullptr;
  esp_err_t err = usb_host_device_open(client, address, &device);
  if (err != ESP_OK || device == nullptr) {
    Serial.printf("[PROBE] Device open failed: %s\n", esp_err_to_name(err));
    return;
  }

  usb_device_info_t info{};
  err = usb_host_device_info(device, &info);
  if (err == ESP_OK) {
    Serial.printf("[PROBE] Address=%u speed=%d\n", address, info.speed);
  }

  const usb_device_desc_t *desc = nullptr;
  err = usb_host_get_device_descriptor(device, &desc);
  if (err == ESP_OK && desc) {
    Serial.printf("[PROBE] VID=0x%04X PID=0x%04X\n", desc->idVendor, desc->idProduct);
  }

  probeConfiguration(device);

  err = usb_host_device_close(client, device);
  if (err != ESP_OK) {
    Serial.printf("[PROBE] Device close failed: %s\n", esp_err_to_name(err));
  }
}

static void usbHostTask(void *) {
  usb_host_config_t hostConfig{};
  hostConfig.skip_phy_setup = false;
  hostConfig.intr_flags = ESP_INTR_FLAG_LEVEL1;

  esp_err_t err = usb_host_install(&hostConfig);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    Serial.printf("[USB] Host install failed: %s\n", esp_err_to_name(err));
    vTaskDelete(nullptr);
    return;
  }

  usb_host_client_config_t clientConfig{};
  clientConfig.is_synchronous = false;
  clientConfig.max_num_event_msg = 8;
  clientConfig.async.client_event_callback = [](const usb_host_client_event_msg_t *msg, void *arg) {
    if (!msg) return;
    auto client = static_cast<usb_host_client_handle_t>(arg);

    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
      Serial.printf("\n[USB] NEW DEVICE address=%u\n", msg->new_dev.address);
      inspectDevice(msg->new_dev.address, client);
    } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
      Serial.println("[USB] DEVICE GONE");
    }
  };

  usb_host_client_handle_t client = nullptr;
  clientConfig.async.callback_arg = nullptr;

  err = usb_host_client_register(&clientConfig, &client);
  if (err != ESP_OK) {
    Serial.printf("[USB] Client register failed: %s\n", esp_err_to_name(err));
    vTaskDelete(nullptr);
    return;
  }

  // The callback needs the registered client handle. It is available only
  // after registration, so update callback_arg before entering the event loop.
  // The Arduino-ESP32 callback structure is copied by the registration call on
  // some core versions; therefore this diagnostic intentionally does not try
  // to mutate it after registration. Existing-device probing is consequently
  // performed by the normal NEW_DEV event path.

  Serial.println("[USB] Host ready. Connect the HP printer.");
  Serial.println("[USB] This test only analyzes descriptors; it does not claim or print.");

  while (true) {
    err = usb_host_lib_handle_events(portMAX_DELAY, nullptr);
    if (err != ESP_OK) {
      Serial.printf("[USB] lib event error: %s\n", esp_err_to_name(err));
    }
    err = usb_host_client_handle_events(client, portMAX_DELAY);
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
      Serial.printf("[USB] client event error: %s\n", esp_err_to_name(err));
    }
  }
}

#endif

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("========================================");
  Serial.println(" ESP32-S3 USB PRINTER INTERFACE PROBE");
  Serial.println("========================================");
  Serial.printf("Arduino-ESP32: %s\n", ESP_ARDUINO_VERSION_STR);

#if HP_USB_HOST_API_AVAILABLE
  xTaskCreatePinnedToCore(usbHostTask, "usb_probe", 8192, nullptr, 5, nullptr, 0);
#else
  Serial.println("[ERROR] usb/usb_host.h is unavailable.");
#endif
}

void loop() {
  delay(1000);
}
