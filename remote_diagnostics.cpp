#include "remote_diagnostics.h"

#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <stdarg.h>

namespace {
constexpr char BLE_DEVICE_NAME[] = "HP-Print-Diagnostics";
constexpr char BLE_SERVICE_UUID[] = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr char BLE_RX_UUID[] = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr char BLE_TX_UUID[] = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr size_t COMMAND_SIZE = 64;
constexpr size_t LOG_RING_SIZE = 2560;

BLECharacteristic *txCharacteristic = nullptr;
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

class ServerCallbacks final : public BLEServerCallbacks {
  void onConnect(BLEServer *) override {
    bleConnected = true;
  }

  void onDisconnect(BLEServer *server) override {
    bleConnected = false;
    server->startAdvertising();
  }
};

class RxCallbacks final : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *characteristic) override {
    const String value = characteristic->getValue();
    const size_t n = min((size_t)value.length(), COMMAND_SIZE - 1);
    memcpy(pendingCommand, value.c_str(), n);
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
  BLEDevice::init(BLE_DEVICE_NAME);
  BLEServer *server = BLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  BLEService *service = server->createService(BLE_SERVICE_UUID);
  txCharacteristic = service->createCharacteristic(
      BLE_TX_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  txCharacteristic->addDescriptor(new BLE2902());

  BLECharacteristic *rx = service->createCharacteristic(
      BLE_RX_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  rx->setCallbacks(new RxCallbacks());

  service->start();
  server->getAdvertising()->addServiceUUID(BLE_SERVICE_UUID);
  server->getAdvertising()->start();
  Serial.printf("[BLE] Diagnostics ready: %s; use a BLE GATT monitor\n", BLE_DEVICE_NAME);
}

void setCommandHook(CommandHook hook) {
  commandHook = hook;
}

void poll() {
  if (!commandReady) return;
  char command[COMMAND_SIZE];
  memcpy(command, pendingCommand, sizeof(command));
  commandReady = false;

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
