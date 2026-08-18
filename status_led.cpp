#include "status_led.h"

namespace {
StatusLed::State current = StatusLed::BOOT;
unsigned long lastMs = 0;
bool phase = false;
#if defined(RGB_BUILTIN)
constexpr int RGB_PIN = RGB_BUILTIN;
#elif defined(LED_BUILTIN)
constexpr int RGB_PIN = LED_BUILTIN;
#else
constexpr int RGB_PIN = -1;
#endif

void rgb(uint8_t r, uint8_t g, uint8_t b) {
#if defined(RGB_BUILTIN)
  rgbLedWrite(RGB_PIN, r, g, b);
#elif defined(LED_BUILTIN)
  digitalWrite(RGB_PIN, (r || g || b) ? HIGH : LOW);
#else
  (void)r; (void)g; (void)b;
#endif
}
}

void StatusLed::begin() {
#if !defined(RGB_BUILTIN) && defined(LED_BUILTIN)
  pinMode(RGB_PIN, OUTPUT);
#endif
  current = BOOT; lastMs = millis(); phase = false; update();
}

void StatusLed::set(State state) {
  if (current != state) { current = state; phase = false; lastMs = millis(); }
  update();
}

void StatusLed::update() {
  const unsigned long now = millis();
  unsigned long interval = 0;
  bool blink = false;
  switch (current) {
    case WIFI_CONNECTING: blink = true; interval = 250; break;
    case WAITING_FOR_PRINTER: blink = true; interval = 1000; break;
    case PRINTING: blink = true; interval = 120; break;
    case ERROR: blink = true; interval = 700; break;
    default: break;
  }
  if (blink && now - lastMs >= interval) { lastMs = now; phase = !phase; }

  switch (current) {
    case BOOT: rgb(255, 255, 255); break;
    case WIFI_CONNECTING: rgb(phase ? 255 : 0, phase ? 120 : 0, 0); break;
    case WIFI_CONNECTED: rgb(0, 255, 0); break;
    case WAITING_FOR_PRINTER: rgb(0, phase ? 120 : 0, phase ? 255 : 0); break;
    case PRINTER_READY: rgb(0, 80, 255); break;
    case PRINTING: rgb(phase ? 180 : 0, 0, phase ? 255 : 0); break;
    case ERROR: rgb(phase ? 255 : 0, 0, 0); break;
  }
}
