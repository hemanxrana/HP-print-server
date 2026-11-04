#include "usb_host_manager.h"
#include "logger.h"
#include "config.h"
#include "esp_task_wdt.h"
#include "esp_intr_alloc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/usb_host.h"
#include "usb/usb_helpers.h"
#include "usb/usb_types_ch9.h"
#include <cstring>
#include <cassert>

namespace {
static constexpr uint8_t USB_PRINTER_CLASS_CODE = 0x07;
static constexpr uint8_t USB_PRINTER_SUBCLASS_CODE = 0x01;
static constexpr uint8_t USB_ENDPOINT_XFER_BULK_CODE = 0x02;
static constexpr uint8_t USB_PRINTER_GET_PORT_STATUS = 0x01;
static constexpr uint8_t USB_PRINTER_STATUS_REQUEST_TYPE = 0xA1;
static constexpr int MAX_ALREADY_CONNECTED_DEVICES = 8;
// 250 ms keeps post-job "fresh status" waits short without hammering the
// printer control endpoint (4 GET_PORT_STATUS/s while attached).
static constexpr uint32_t STATUS_POLL_INTERVAL_MS = 250;
static constexpr uint32_t STATUS_TRANSFER_TIMEOUT_MS = 1000;
// Persistent Bulk OUT buffer size comes from config.h so the chunk size and the
// transfer buffer can never drift apart (a mismatch rejects every chunk).
static constexpr size_t BULK_OUT_BUFFER_SIZE = BULK_OUT_CHUNK_SIZE;

// Pipelined Bulk OUT: up to USB_BULK_PIPELINE_DEPTH transfers are submitted
// ahead so the USB bus never idles between chunks.
struct BulkOutSlot {
  usb_transfer_t *transfer = nullptr;
  SemaphoreHandle_t done = nullptr;
  volatile bool inFlight = false;
  usb_transfer_status_t status = USB_TRANSFER_STATUS_NO_DEVICE;
  size_t accepted = 0;
};

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

  BulkOutSlot bulkOut[USB_BULK_PIPELINE_DEPTH];
  uint8_t bulkOutNext = 0;
  volatile bool bulkOutWedged = false;
  volatile bool resetRequested = false;
  unsigned long rescanAtMs = 0;
  uint8_t rescanAttempts = 0;

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

static void clearBulkOutInFlight() {
  for (int i = 0; i < USB_BULK_PIPELINE_DEPTH; ++i) g.bulkOut[i].inFlight = false;
}

static void freeBulkOutResources() {
  for (int i = 0; i < USB_BULK_PIPELINE_DEPTH; ++i) {
    BulkOutSlot &s = g.bulkOut[i];
    if (s.transfer && !s.inFlight) {
      usb_host_transfer_free(s.transfer);
      s.transfer = nullptr;
    }
    if (s.done && !s.transfer) {
      vSemaphoreDelete(s.done);
      s.done = nullptr;
    }
  }
}

static bool ensureBulkOutResources(String &error) {
  if (g.bulkOutWedged) {
    error = "USB Bulk OUT endpoint wedged; recovering USB link";
    return false;
  }
  for (int i = 0; i < USB_BULK_PIPELINE_DEPTH; ++i) {
    BulkOutSlot &s = g.bulkOut[i];
    if (!s.done) {
      s.done = xSemaphoreCreateBinary();
      if (!s.done) {
        error = "unable to allocate persistent USB completion semaphore";
        return false;
      }
    }
    if (!s.transfer) {
      const esp_err_t e = usb_host_transfer_alloc(BULK_OUT_BUFFER_SIZE, 0, &s.transfer);
      if (e != ESP_OK || !s.transfer) {
        error = String("persistent usb_host_transfer_alloc failed: ") + esp_err_to_name(e);
        vSemaphoreDelete(s.done);
        s.done = nullptr;
        return false;
      }
    }
  }
  static bool announced = false;
  if (!announced) {
    Log.printf("[USB] Persistent Bulk OUT pipeline: %u transfer buffers of %u bytes\n",
                  (unsigned)USB_BULK_PIPELINE_DEPTH, (unsigned)BULK_OUT_BUFFER_SIZE);
    announced = true;
  }
  return true;
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
  freeBulkOutResources();
}

static void releaseInterfaces() {
  if (!g.deviceOpen || !g.device) return;
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
      Log.printf("[USB] Status interface IF=%u ALT=%u could not be claimed: %s; using print interface for status\n",
                    status.interfaceNumber, status.alternateSetting,
                    esp_err_to_name(e));
    }
  }

  if (!d.statusInterfaceSeparate) d.statusInterface = d.printer;

  Log.printf("[USB] Selected Printer Class IF=%u ALT=%u protocol=0x%02X OUT=0x%02X IN=0x%02X\n",
                d.printer.interfaceNumber, d.printer.alternateSetting,
                d.printer.protocol, d.printer.bulkOut.address,
                d.printer.bulkIn.address);
  if (d.statusInterfaceSeparate) {
    Log.printf("[USB] Status IF=%u ALT=%u protocol=0x%02X (separate)\n",
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
          Log.printf("[USB] Ignoring IF=%u ALT=%u: protocol 0x04 is IPP-over-USB, not RAW printing\n",
                        i->bInterfaceNumber, i->bAlternateSetting);
        } else if (i->bInterfaceProtocol == 0xFF) {
          Log.printf("[USB] Ignoring IF=%u ALT=%u: vendor-specific Printer Class protocol 0xFF is not verified for RAW\n",
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
    Log.printf("[USB] RAW candidate %u: IF=%u ALT=%u protocol=0x%02X OUT=0x%02X IN=0x%02X score=%d\n",
                  i, p.interfaceNumber, p.alternateSetting, p.protocol,
                  p.bulkOut.address, p.bulkIn.address, printScore(p));
  }

  if (!claimInterfaces(out, error)) {
    closeDevice();
    return false;
  }
  return true;
}

static void publish(const UsbDeviceInfo &d) {
  g.rescanAttempts = 0;
  if (!manager) return;
  manager->onEnumerated(d);
  Log.printf("[USB] Printer ready: %s VID=0x%04X PID=0x%04X address=%u\n",
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
    Log.printf("[USB] Ignored address %u: %s\n", addresses[i], error.c_str());
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
  BulkOutSlot *slot = static_cast<BulkOutSlot *>(t->context);
  if (!slot) return;
  slot->inFlight = false;
  slot->status = t->status;
  slot->accepted = static_cast<size_t>(t->actual_num_bytes);
  if (slot->done) xSemaphoreGive(slot->done);
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

    if (g.resetRequested) {
      g.resetRequested = false;
      Log.println("[USB] Recovering from unrecoverable Bulk OUT transfer; re-enumerating");
      closeDevice();
      if (manager) manager->onDetached();
      clearBulkOutInFlight();
      g.bulkOutWedged = false;
      scanExisting();
      g.rescanAtMs = millis() + 2000;
      g.rescanAttempts = 15;
    }
    if (!g.deviceOpen && g.rescanAttempts && (int32_t)(millis() - g.rescanAtMs) >= 0) {
      g.rescanAtMs = millis() + 2000;
      --g.rescanAttempts;
      scanExisting();
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
        Log.printf("[USB] Ignored address %u: %s\n", address, error.c_str());
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

namespace {
static bool waitSlotDone(BulkOutSlot &s, usb_transfer_t *t, uint32_t timeoutMs,
                         const char *what, String &error) {
  const TickType_t waitTicks = timeoutMs ? pdMS_TO_TICKS(timeoutMs + 250) : portMAX_DELAY;
  if (xSemaphoreTake(s.done, waitTicks) != pdTRUE) {
    Log.printf("[USB] Bulk OUT %s wait exceeded transfer timeout; recovering endpoint\n", what);
    usb_host_endpoint_halt(t->device_handle, t->bEndpointAddress);
    usb_host_endpoint_flush(t->device_handle, t->bEndpointAddress);
    usb_host_endpoint_clear(t->device_handle, t->bEndpointAddress);
    if (xSemaphoreTake(s.done, pdMS_TO_TICKS(1500)) != pdTRUE) {
      g.bulkOutWedged = true;
      g.resetRequested = true;
      error = String("USB Bulk OUT ") + what + " unrecoverable; re-enumerating USB link";
      return false;
    }
    error = String("USB Bulk OUT ") + what + " timed out; endpoint recovered";
    return false;
  }
  if (s.status != USB_TRANSFER_STATUS_COMPLETED || s.accepted != t->num_bytes) {
    Log.printf("[USB] Bulk OUT %s failed status=%d accepted=%u/%u; recovering endpoint\n",
               what, (int)s.status, (unsigned)s.accepted, (unsigned)t->num_bytes);
    usb_host_endpoint_halt(t->device_handle, t->bEndpointAddress);
    usb_host_endpoint_flush(t->device_handle, t->bEndpointAddress);
    usb_host_endpoint_clear(t->device_handle, t->bEndpointAddress);
    g.bulkOutWedged = true;
    g.resetRequested = true;
    error = String("USB bulk transfer failed status=") + String((int)s.status) +
            " accepted=" + String((unsigned)s.accepted) + "/" + String((unsigned)t->num_bytes);
    return false;
  }
  return true;
}
} // namespace

bool UsbHostManager::bulkWrite(const uint8_t *data, size_t length, size_t &accepted,
                               uint32_t timeoutMs, String &error) {
  accepted = 0;
  if (!data || !length) { error = "empty USB transfer"; return false; }
  if (!g.deviceOpen || !g.printInterfaceClaimed || !device_.printer.usableForRawPrint()) {
    error = "USB printer interface is not claimed/ready"; return false;
  }
  if (length > BULK_OUT_BUFFER_SIZE) {
    error = String("USB transfer chunk exceeds persistent buffer: ") + String((unsigned)length) +
            "/" + String((unsigned)BULK_OUT_BUFFER_SIZE); return false;
  }
  if (g.bulkOutWedged) { error = "USB Bulk OUT endpoint wedged; recovering USB link"; return false; }
  if (!ensureBulkOutResources(error)) return false;

  const int mine = g.bulkOutNext;
  const int previous = (mine + USB_BULK_PIPELINE_DEPTH - 1) % USB_BULK_PIPELINE_DEPTH;
  BulkOutSlot &prev = g.bulkOut[previous];

  // Reap the previous in-flight transfer here, AFTER submitting the next one,
  // so multiple chunks are always in flight back-to-back on the bus.
  if (prev.inFlight && !waitSlotDone(prev, prev.transfer, timeoutMs, "pipelined", error)) {
    prev.inFlight = false;
    if (g.bulkOutWedged) { g.bulkOutNext = mine; return false; }
  }

  BulkOutSlot &slot = g.bulkOut[mine];
  while (xSemaphoreTake(slot.done, 0) == pdTRUE) {}
  slot.status = USB_TRANSFER_STATUS_NO_DEVICE;
  slot.accepted = 0;

  usb_transfer_t *t = slot.transfer;
  assert(t && !slot.inFlight);
  memcpy(t->data_buffer, data, length);
  t->num_bytes = length;
  t->device_handle = g.device;
  t->bEndpointAddress = device_.printer.bulkOut.address;
  t->callback = bulkTransferCallback;
  t->context = &slot;
  t->timeout_ms = timeoutMs;

  const esp_err_t e = usb_host_transfer_submit(t);
  if (e != ESP_OK) {
    error = String("USB bulk transfer submit failed: ") + esp_err_to_name(e);
    usb_host_endpoint_halt(t->device_handle, t->bEndpointAddress);
    usb_host_endpoint_flush(t->device_handle, t->bEndpointAddress);
    usb_host_endpoint_clear(t->device_handle, t->bEndpointAddress);
    return false;
  }
  slot.inFlight = true;
  g.bulkOutNext = (uint8_t)((mine + 1) % USB_BULK_PIPELINE_DEPTH);

  // The submission is the acceptance point: bytes handed to the USB host
  // driver are transmitted even if this call detects a failed predecessor.
  accepted = length;
  return true;
}

bool UsbHostManager::bulkWriteAll(const uint8_t *data, size_t length,
                                  uint32_t timeoutMs, String &error) {
  if (!data || !length) return true;
  size_t offset = 0;
  int retries = 0;
  while (offset < length) {
    const size_t part = min(static_cast<size_t>(BULK_OUT_BUFFER_SIZE), length - offset);
    size_t accepted = 0;
    esp_task_wdt_reset();
    if (!bulkWrite(data + offset, part, accepted, timeoutMs, error)) {
      if (accepted > 0) {
        offset += accepted;
        retries = 0;
        continue;
      }
      if (++retries > 3) return false;
      delay(50);
      continue;
    }
    offset += accepted;
    retries = 0;
    yield();
  }
  return true;
}

bool UsbHostManager::waitForBulkOut(uint32_t timeoutMs, String &error) {
  if (g.bulkOutWedged) { error = "USB Bulk OUT endpoint wedged; recovering USB link"; return false; }
  if (!ensureBulkOutResources(error)) return false;
  // Cap each slot wait so the sequential drain of all pipelined transfers
  // (including the recovery grace window) stays well below the watchdog.
  const uint32_t slotTimeout = timeoutMs / USB_BULK_PIPELINE_DEPTH;
  bool ok = true;
  for (int i = 0; i < USB_BULK_PIPELINE_DEPTH && ok; ++i) {
    esp_task_wdt_reset();
    BulkOutSlot &s = g.bulkOut[i];
    if (!s.inFlight) continue;
    if (waitSlotDone(s, s.transfer, slotTimeout, "drain", error)) continue;
    s.inFlight = false;
    ok = false;
  }
  return ok;
}

bool UsbHostManager::bulkOutWedged() const {
  return g.bulkOutWedged;
}

void UsbHostManager::onPortStatusTransfer(bool valid, uint8_t value, const String &error) {
  static String lastStatusError;
  if (!valid) {
    if (error.length() && error != lastStatusError) {
      Log.printf("[USB] GET_PORT_STATUS error: %s\n", error.c_str());
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
    Log.printf("[USB] GET_PORT_STATUS IF=%u value=0x%02X error=%d selected=%d paper-empty=%d\n",
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
  clearBulkOutInFlight();
  g.bulkOutWedged = false;
  state_ = info.printer.found ? PRINTER_READY : DEVICE_ATTACHED;
}

void UsbHostManager::onDetached() {
  if (device_.attached) Log.println("[USB] Device disconnected");
  device_ = UsbDeviceInfo{};
  error_.clear();
  state_ = started_ ? RUNNING : STOPPED;
}

void UsbHostManager::onEnumerationError(const String &error) {
  error_ = error;
  if (started_ && state_ != ERROR) state_ = RUNNING;
}