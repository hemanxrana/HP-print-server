#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include "usb_host_manager.h"
#include "usb_printer_backend.h"

// ESP32-S3 Arduino-ESP32 3.x RGB status LED support.
// Uses the core's RGB_BUILTIN/rgbLedWrite() API; no extra library is required.

extern WebServer configServer;
extern String dashboard();
extern Preferences preferences;
extern UsbHostManager usbHost;
extern UsbPrinterBackend usbPrinterBackend;

#ifdef RGB_BUILTIN
namespace {
constexpr const char *LED_NS = "hp-print";
constexpr uint8_t DEFAULT_BRIGHTNESS = 48;

struct LedColor { uint8_t r, g, b; };
struct LedConfig {
  LedColor wifiConnected{0, 255, 0};
  LedColor wifiDisconnected{255, 128, 0};
  LedColor printing{0, 96, 255};
  LedColor error{255, 0, 0};
  LedColor usbReady{0, 255, 0};
  LedColor usbWaiting{255, 180, 0};
  LedColor apMode{0, 180, 255};
  uint8_t brightness = DEFAULT_BRIGHTNESS;
  bool enabled = true;
};

LedConfig ledConfig;
volatile bool ledTaskStarted = false;

uint8_t clampByte(int v) { return (uint8_t)constrain(v, 0, 255); }

LedColor readColor(Preferences &p, const char *key, LedColor fallback) {
  String value = p.getString(key, "");
  if (value.length() != 7 || value[0] != '#') return fallback;
  char *end = nullptr;
  unsigned long n = strtoul(value.substring(1).c_str(), &end, 16);
  if (!end || *end != '\0') return fallback;
  return {(uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n};
}

String colorHex(LedColor c) {
  char b[8];
  snprintf(b, sizeof(b), "#%02X%02X%02X", c.r, c.g, c.b);
  return String(b);
}

void loadLedConfig() {
  if (!preferences.begin(LED_NS, true)) return;
  ledConfig.wifiConnected = readColor(preferences, "led_wifi", ledConfig.wifiConnected);
  ledConfig.wifiDisconnected = readColor(preferences, "led_disc", ledConfig.wifiDisconnected);
  ledConfig.printing = readColor(preferences, "led_print", ledConfig.printing);
  ledConfig.error = readColor(preferences, "led_error", ledConfig.error);
  ledConfig.usbReady = readColor(preferences, "led_usb", ledConfig.usbReady);
  ledConfig.usbWaiting = readColor(preferences, "led_wait", ledConfig.usbWaiting);
  ledConfig.apMode = readColor(preferences, "led_ap", ledConfig.apMode);
  ledConfig.brightness = preferences.getUChar("led_bright", DEFAULT_BRIGHTNESS);
  ledConfig.enabled = preferences.getBool("led_on", true);
  preferences.end();
}

void saveLedConfig() {
  if (!preferences.begin(LED_NS, false)) return;
  preferences.putString("led_wifi", colorHex(ledConfig.wifiConnected));
  preferences.putString("led_disc", colorHex(ledConfig.wifiDisconnected));
  preferences.putString("led_print", colorHex(ledConfig.printing));
  preferences.putString("led_error", colorHex(ledConfig.error));
  preferences.putString("led_usb", colorHex(ledConfig.usbReady));
  preferences.putString("led_wait", colorHex(ledConfig.usbWaiting));
  preferences.putString("led_ap", colorHex(ledConfig.apMode));
  preferences.putUChar("led_bright", ledConfig.brightness);
  preferences.putBool("led_on", ledConfig.enabled);
  preferences.end();
}

void writeLed(LedColor c) {
  if (!ledConfig.enabled) {
    rgbLedWrite(RGB_BUILTIN, 0, 0, 0);
    return;
  }
  const uint16_t scale = ledConfig.brightness;
  rgbLedWrite(RGB_BUILTIN,
              (uint8_t)((uint16_t)c.r * scale / 255),
              (uint8_t)((uint16_t)c.g * scale / 255),
              (uint8_t)((uint16_t)c.b * scale / 255));
}

LedColor getArgColor(const char *name, LedColor fallback) {
  if (!configServer.hasArg(name)) return fallback;
  String v = configServer.arg(name); v.trim();
  if (v.length() != 7 || v[0] != '#') return fallback;
  char *end = nullptr;
  unsigned long n = strtoul(v.substring(1).c_str(), &end, 16);
  if (!end || *end != '\0') return fallback;
  return {(uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n};
}

String ledPage() {
  String h;
  h.reserve(6500);
  h += F("<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'><title>LED Settings</title>");
  h += F("<style>body{font-family:system-ui,Arial;max-width:820px;margin:24px auto;padding:0 16px;background:#f5f5f5;color:#222}section{background:#fff;padding:20px;margin:16px 0;border-radius:12px;box-shadow:0 2px 8px #0001}label{display:flex;align-items:center;gap:12px;margin:12px 0}input[type=color]{width:64px;height:40px;padding:2px}input[type=range]{width:100%}button{padding:11px 18px;border:0;border-radius:7px;background:#222;color:#fff}</style></head><body>");
  h += F("<h1>Status LED</h1><section><p>The onboard RGB LED uses these colors for server states. Changes are saved in NVS.</p><form method='POST' action='/led'>");
  h += "<label><input type='checkbox' name='enabled' " + String(ledConfig.enabled ? "checked" : "") + "> LED enabled</label>";
  h += "<label>Brightness <input type='range' name='brightness' min='1' max='255' value='" + String(ledConfig.brightness) + "'></label>";
  h += "<label>Wi-Fi connected <input type='color' name='wifi' value='" + colorHex(ledConfig.wifiConnected) + "'></label>";
  h += "<label>Wi-Fi disconnected <input type='color' name='disc' value='" + colorHex(ledConfig.wifiDisconnected) + "'></label>";
  h += "<label>Printing <input type='color' name='print' value='" + colorHex(ledConfig.printing) + "'></label>";
  h += "<label>Printer/USB error <input type='color' name='error' value='" + colorHex(ledConfig.error) + "'></label>";
  h += "<label>USB printer ready <input type='color' name='usb' value='" + colorHex(ledConfig.usbReady) + "'></label>";
  h += "<label>USB waiting/not ready <input type='color' name='wait' value='" + colorHex(ledConfig.usbWaiting) + "'></label>";
  h += "<label>Configuration AP <input type='color' name='ap' value='" + colorHex(ledConfig.apMode) + "'></label>";
  h += F("<button type='submit'>Save LED settings</button></form></section><section><h2>Current state</h2><p>Priority: error → printing → Wi-Fi/AP → USB ready/waiting.</p><p><a href='/'>Back to HP Print Server</a></p></section></body></html>");
  return h;
}

void handleLedGet() { configServer.send(200, "text/html; charset=utf-8", ledPage()); }

void handleLedPost() {
  ledConfig.enabled = configServer.hasArg("enabled");
  if (configServer.hasArg("brightness")) ledConfig.brightness = clampByte(configServer.arg("brightness").toInt());
  if (ledConfig.brightness == 0) ledConfig.brightness = 1;
  ledConfig.wifiConnected = getArgColor("wifi", ledConfig.wifiConnected);
  ledConfig.wifiDisconnected = getArgColor("disc", ledConfig.wifiDisconnected);
  ledConfig.printing = getArgColor("print", ledConfig.printing);
  ledConfig.error = getArgColor("error", ledConfig.error);
  ledConfig.usbReady = getArgColor("usb", ledConfig.usbReady);
  ledConfig.usbWaiting = getArgColor("wait", ledConfig.usbWaiting);
  ledConfig.apMode = getArgColor("ap", ledConfig.apMode);
  saveLedConfig();
  writeLed(ledConfig.wifiConnected);
  configServer.send(200, "text/html; charset=utf-8", "<p>LED settings saved.</p><p><a href='/led'>LED settings</a> &nbsp; <a href='/'>Home</a></p>");
}

void handleLedRoot() {
  String h = dashboard();
  const String marker = "</body>";
  const int pos = h.lastIndexOf(marker);
  const String card = "<section style='background:#fff;padding:20px;margin:16px 0;border-radius:12px'><h2>Status LED</h2><p>Configure RGB colors for Wi-Fi, printing, USB and error states.</p><p><a href='/led'>Open LED color settings</a></p></section>";
  if (pos >= 0) h = h.substring(0, pos) + card + h.substring(pos);
  configServer.send(200, "text/html; charset=utf-8", h);
}

void ledTask(void *) {
  bool blink = false;
  uint32_t last = 0;
  for (;;) {
    const uint32_t now = millis();
    if (now - last >= 400) { last = now; blink = !blink; }

    LedColor c = ledConfig.wifiDisconnected;
    if (usbPrinterBackend.state() == UsbPrinterBackend::ERROR || usbHost.state() == UsbHostManager::ERROR) {
      c = ledConfig.error;
      if (!blink) c = {0, 0, 0};
    } else if (usbPrinterBackend.state() == UsbPrinterBackend::PRINTING) {
      c = ledConfig.printing;
    } else if (WiFi.getMode() == WIFI_AP && WiFi.status() != WL_CONNECTED) {
      c = ledConfig.apMode;
    } else if (WiFi.status() == WL_CONNECTED) {
      c = usbPrinterBackend.online() ? ledConfig.usbReady : ledConfig.usbWaiting;
    }
    writeLed(c);
    vTaskDelay(pdMS_TO_TICKS(80));
  }
}
}

void initVariant() {
  loadLedConfig();
  rgbLedWrite(RGB_BUILTIN, 0, 0, 0);
  configServer.on("/led", HTTP_GET, handleLedGet);
  configServer.on("/led", HTTP_POST, handleLedPost);
  configServer.on("/", HTTP_GET, handleLedRoot);
  if (!ledTaskStarted) {
    ledTaskStarted = true;
    xTaskCreatePinnedToCore(ledTask, "hp-led", 4096, nullptr, 1, nullptr, 0);
  }
}
#else
void initVariant() {}
#endif
