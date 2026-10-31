#include "remote_diagnostics.h"

#include "logger.h"

#include <NimBLEDevice.h>
#include <esp_heap_caps.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace {
constexpr char DEFAULT_BLE_NAME[] = "HP-Print-Diagnostics";
constexpr char BLE_SERVICE_UUID[] = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr char BLE_RX_UUID[] = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr char BLE_TX_UUID[] = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr size_t COMMAND_SIZE = 64;
constexpr size_t LOG_RING_SIZE = 64 * 1024;
constexpr size_t LOG_FALLBACK_SIZE = 4096;
constexpr size_t BLE_LOG_TAIL = 1200;
constexpr size_t BLE_NAME_LIMIT = 24;

NimBLECharacteristic *txCharacteristic = nullptr;
volatile bool bleConnected = false;
volatile bool bleInitialized = false;
volatile bool commandReady = false;
char pendingCommand[COMMAND_SIZE] = {};

String bleName = DEFAULT_BLE_NAME;
bool bleEnabled = true;

RemoteDiagnostics::CommandHook commandHook = nullptr;
char fallbackRing[LOG_FALLBACK_SIZE] = {};
char *logRing = fallbackRing;
size_t logCapacity = LOG_FALLBACK_SIZE;
size_t logLength = 0;
uint32_t logWritten = 0;
bool ringInPsram = false;
SemaphoreHandle_t logMutex = nullptr;

SemaphoreHandle_t logLockHandle() {
  return logMutex;
}

struct LogLock {
  SemaphoreHandle_t handle;
  LogLock() : handle(logLockHandle()) {
    if (handle) xSemaphoreTake(handle, portMAX_DELAY);
  }
  ~LogLock() {
    if (handle) xSemaphoreGive(handle);
  }
};

void initRing() {
  char *psramRing = (char *)heap_caps_malloc(LOG_RING_SIZE, MALLOC_CAP_SPIRAM);
  if (psramRing) {
    logRing = psramRing;
    logCapacity = LOG_RING_SIZE;
    ringInPsram = true;
  }
  logLength = 0;
  logWritten = 0;
  logRing[0] = '\0';
}

void appendUnlocked(const char *source, size_t length) {
  if (!source || !length) return;
  const size_t capacity = logCapacity - 1;
  if (length > capacity) {
    source += (length - capacity);
    length = capacity;
  }
  if (logLength + length > capacity) {
    const size_t drop = logLength + length - capacity;
    memmove(logRing, logRing + drop, logLength - drop);
    logLength -= drop;
  }
  memcpy(logRing + logLength, source, length);
  logLength += length;
  logRing[logLength] = '\0';
  logWritten += (uint32_t)length;
}

void appendLog(const String &line) {
  LogLock lock;
  appendUnlocked(line.c_str(), line.length());
}

class ServerCallbacks final : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *, NimBLEConnInfo &) override {
    bleConnected = true;
  }

  void onDisconnect(NimBLEServer *, NimBLEConnInfo &, int) override {
    bleConnected = false;
    if (!bleInitialized) return;
    NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
    if (advertising) advertising->start();
  }
};

class RxCallbacks final : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *characteristic, NimBLEConnInfo &) override {
    const std::string value = characteristic->getValue();
    const size_t n = min((size_t)value.size(), COMMAND_SIZE - 1);
    memcpy(pendingCommand, value.data(), n);
    pendingCommand[n] = '\0';
    commandReady = true;
  }
};

void notifyLine(const String &line) {
  if (!bleConnected || !txCharacteristic || !bleInitialized) return;
  constexpr size_t BLE_PAYLOAD = 20;
  for (size_t offset = 0; offset < line.length(); offset += BLE_PAYLOAD) {
    const size_t count = min(BLE_PAYLOAD, line.length() - offset);
    txCharacteristic->setValue((uint8_t *)line.c_str() + offset, count);
    txCharacteristic->notify();
    delay(2);
  }
}

String sanitizeName(const String &name) {
  String clean = name;
  clean.trim();
  if (clean.isEmpty()) clean = DEFAULT_BLE_NAME;
  if (clean.length() > BLE_NAME_LIMIT) clean = clean.substring(0, BLE_NAME_LIMIT);
  return clean;
}

void startBle() {
  if (bleInitialized || !bleEnabled) return;
  NimBLEDevice::init(bleName.c_str());
  NimBLEServer *server = NimBLEDevice::createServer();
  if (!server) {
    Log.println("[BLE] createServer failed");
    NimBLEDevice::deinit(true);
    return;
  }
  server->setCallbacks(new ServerCallbacks());
  NimBLEService *service = server->createService(BLE_SERVICE_UUID);
  if (!service) {
    Log.println("[BLE] createService failed");
    NimBLEDevice::deinit(true);
    return;
  }
  txCharacteristic = service->createCharacteristic(BLE_TX_UUID, NIMBLE_PROPERTY::NOTIFY);
  NimBLECharacteristic *rx = service->createCharacteristic(
      BLE_RX_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  rx->setCallbacks(new RxCallbacks());
  // NimBLE creates the 0x2902 client-configuration descriptor automatically;
  // manually adding one is deprecated in NimBLE-Arduino 2.x.
  service->start();

  NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
  if (advertising) {
    advertising->addServiceUUID(BLE_SERVICE_UUID);
    advertising->setName(bleName.c_str());
    advertising->enableScanResponse(true);
    advertising->start();
  }
  bleInitialized = true;
  Log.printf("[BLE] Diagnostics ready: %s; use a BLE GATT monitor\n", bleName.c_str());
}

void stopBle() {
  if (!bleInitialized) return;
  bleInitialized = false;
  bleConnected = false;
  txCharacteristic = nullptr;
  NimBLEDevice::deinit(true);
  Log.println("[BLE] Diagnostics disabled");
}
} // namespace

namespace RemoteDiagnostics {
void begin() {
  if (!logMutex) logMutex = xSemaphoreCreateMutex();
  initRing();
  Log.printf("[LOG] %u-byte log ring in %s; read it at /log, /monitor or TCP 23\n",
             (unsigned)logCapacity, ringInPsram ? "PSRAM" : "internal RAM");
}

void configure(bool enabled, const String &deviceName) {
  const String wanted = sanitizeName(deviceName);
  const bool nameChanged = (wanted != bleName);
  bleEnabled = enabled;
  bleName = wanted;
  if (!bleEnabled) {
    stopBle();
    return;
  }
  if (bleInitialized && nameChanged) stopBle();
  startBle();
}

bool enabled() {
  return bleEnabled;
}

bool ready() {
  return bleInitialized;
}

bool connected() {
  return bleConnected;
}

String deviceName() {
  return bleName;
}

void setCommandHook(CommandHook hook) {
  commandHook = hook;
}

void poll() {
  if (!commandReady) return;
  char command[COMMAND_SIZE];
  noInterrupts();
  memcpy(command, pendingCommand, sizeof(command));
  commandReady = false;
  interrupts();
  String normalized(command);
  normalized.trim();
  normalized.toLowerCase();
  if (normalized == "help") {
    publish("commands: help, ping, status, log, counters, job\n");
  } else if (normalized == "ping") {
    publish("pong\n");
  } else if (normalized == "status") {
    publish("status is available from the Wi-Fi dashboard\n");
  } else if (normalized.length() && commandHook) {
    commandHook(normalized);
  } else if (normalized.length()) {
    publish("unknown command; use help\n");
  }
}

void publish(const String &message) {
  appendLog(message);
  notifyLine(message);
}

void publishf(const char *format, ...) {
  char buffer[256];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  publish(String(buffer));
}

void appendRaw(const char *data, size_t length) {
  LogLock lock;
  appendUnlocked(data, length);
}

const char *logData(size_t &length) {
  length = logLength;
  return logRing;
}

uint32_t logSequence() {
  LogLock lock;
  return logWritten;
}

bool logInPsram() {
  return ::ringInPsram;
}

// Only a tail is returned because the ring can be 64 KB and BLE/notifications
// and String copies are not meant to move that much data.
String recentLog() {
  LogLock lock;
  const size_t tail = min(logLength, BLE_LOG_TAIL);
  return String(logRing + (logLength - tail));
}
} // namespace RemoteDiagnostics
