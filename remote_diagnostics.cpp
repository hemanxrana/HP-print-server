#include "remote_diagnostics.h"

#include <NimBLEDevice.h>
#include <stdarg.h>

namespace {
constexpr char BLE_DEVICE_NAME[] = "HP-Print-Diagnostics";
constexpr char BLE_SERVICE_UUID[] = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr char BLE_RX_UUID[] = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr char BLE_TX_UUID[] = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr size_t COMMAND_SIZE = 64;
constexpr size_t LOG_RING_SIZE = 2560;

NimBLECharacteristic *txCharacteristic = nullptr;
volatile bool bleConnected = false;
volatile bool commandReady = false;
char pendingCommand[COMMAND_SIZE] = {};

RemoteDiagnostics::CommandHook commandHook = nullptr;
char logRing[LOG_RING_SIZE] = {};
size_t logLength = 0;

void appendLog(const String &line) {
  const size_t capacity = sizeof(logRing) - 1;
  size_t length = line.length();
  const char *source = line.c_str();
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
}

class ServerCallbacks final : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *, NimBLEConnInfo &) override {
    bleConnected = true;
  }

  void onDisconnect(NimBLEServer *, NimBLEConnInfo &, int) override {
    bleConnected = false;
    NimBLEDevice::getAdvertising()->start();
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
  if (!bleConnected || !txCharacteristic) return;
  constexpr size_t BLE_PAYLOAD = 20;
  for (size_t offset = 0; offset < line.length(); offset += BLE_PAYLOAD) {
    const size_t count = min(BLE_PAYLOAD, line.length() - offset);
    txCharacteristic->setValue((uint8_t *)line.c_str() + offset, count);
    txCharacteristic->notify();
    delay(2);
  }
}
} // namespace

namespace RemoteDiagnostics {
void begin() {
  NimBLEDevice::init(BLE_DEVICE_NAME);
  NimBLEServer *server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());
  NimBLEService *service = server->createService(BLE_SERVICE_UUID);
  txCharacteristic = service->createCharacteristic(BLE_TX_UUID, NIMBLE_PROPERTY::NOTIFY);
  NimBLECharacteristic *rx = service->createCharacteristic(
      BLE_RX_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  rx->setCallbacks(new RxCallbacks());
  // NimBLE creates the 0x2902 client-configuration descriptor automatically;
  // manually adding one is deprecated in NimBLE-Arduino 2.x.
  service->start();

  NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
  advertising->addServiceUUID(BLE_SERVICE_UUID);
  advertising->setName(BLE_DEVICE_NAME);
  advertising->enableScanResponse(true);
  advertising->start();

  Serial.printf("[BLE] Diagnostics ready: %s; use a BLE GATT monitor\n", BLE_DEVICE_NAME);
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

bool connected() {
  return bleConnected;
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

String recentLog() {
  return String(logRing);
}
} // namespace RemoteDiagnostics
