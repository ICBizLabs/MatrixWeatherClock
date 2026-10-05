#include "buttons.h"
#include "pca9557.h"
#include "io/i2c_bus.h"
#include "pins.h"
#include "util/log.h"

namespace buttons {
  namespace {
    Handler handler = nullptr;
    bool ready = false;
    uint8_t idle = 0xFF;                 // level of the key bits when nothing is pressed
    uint8_t stable = 0, lastRaw = 0;
    uint32_t lastPoll = 0, changeAt = 0, pressedAt[3] = {0, 0, 0};
    bool longFired[3] = {false, false, false};
    constexpr uint8_t BITS[3] = { 1 << 1, 1 << 3, 1 << 2 };   // K1 = IO1, K2 = IO3, K3 = IO2
    constexpr uint32_t POLL_MS = 20, DEBOUNCE_MS = 30, LONG_MS = 1500;
  }

  namespace {
    // A single key on a plain GPIO (the C3 board's BOOT key): short press = K1, long press = K2.
    bool gpioKey = false, keyDown = false, keyLong = false;
    uint32_t keyChangeAt = 0, keyDownAt = 0;
    bool keyLevel = true;

    void gpioLoop() {
      const uint32_t now = millis();
      if (now - lastPoll < POLL_MS) return;
      lastPoll = now;
      const bool level = digitalRead(pins::KEY);            // active low
      if (level != keyLevel) { keyLevel = level; keyChangeAt = now; return; }
      if (now - keyChangeAt < DEBOUNCE_MS) return;
      const bool down = !level;
      if (down && !keyDown) { keyDown = true; keyLong = false; keyDownAt = now; }
      else if (!down && keyDown) { keyDown = false; if (!keyLong && handler) handler(K1, false); }
      if (keyDown && !keyLong && now - keyDownAt >= LONG_MS) { keyLong = true; if (handler) handler(K2, false); }
    }
  }

  void begin(Handler h) {
    handler = h;
    if (pins::KEY >= 0) {
      pinMode(pins::KEY, INPUT_PULLUP);
      keyLevel = digitalRead(pins::KEY);
      gpioKey = ready = true;
      LOGI("buttons: key on GPIO %d (tap = next page, hold = dismiss)", pins::KEY);
      return;
    }
    uint8_t addr = i2c_bus::map().pca9557;
    if (!addr || !pca9557::begin(addr)) { LOGW("buttons: no PCA9557, keys disabled"); return; }
    uint8_t v;
    if (pca9557::readInputs(v)) { idle = v; stable = lastRaw = v; }
    ready = true;
  }

  bool available() { return ready; }

  void loop() {
    if (!ready) return;
    if (gpioKey) { gpioLoop(); return; }
    uint32_t now = millis();
    if (now - lastPoll < POLL_MS) return;
    lastPoll = now;
    uint8_t raw;
    if (!pca9557::readInputs(raw)) return;
    if (raw != lastRaw) { lastRaw = raw; changeAt = now; return; }
    if (now - changeAt < DEBOUNCE_MS || raw == stable) {
      // hold detection for long presses
      for (uint8_t k = 0; k < 3; k++) {
        bool pressed = ((stable ^ idle) & BITS[k]) != 0;
        if (pressed && !longFired[k] && now - pressedAt[k] >= LONG_MS) { longFired[k] = true; if (handler) handler(k, true); }
      }
      return;
    }
    uint8_t changed = raw ^ stable;
    stable = raw;
    for (uint8_t k = 0; k < 3; k++) {
      if (!(changed & BITS[k])) continue;
      bool pressed = ((raw ^ idle) & BITS[k]) != 0;
      if (pressed) { pressedAt[k] = now; longFired[k] = false; }
      else if (!longFired[k] && handler) handler(k, false);
    }
  }
}
