#include "buttons.h"
#include "pca9557.h"
#include "io/i2c_bus.h"
#include "pins.h"
#include "actions.h"
#include "alarm/alarm.h"
#include "display/renderer.h"
#include "util/log.h"
#if defined(MWC_BOARD_C3TV)
#include "display/panel.h"
#include "net/net_task.h"
#include <WiFi.h>
#include "driver/gpio.h"
#include "esp_sleep.h"
#endif

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
    // Keys on plain GPIOs (the C3 board: Key1, Key2 and BOOT, all to ground). Each has its own tap and hold.
    // While an alarm or timer rings every key goes to the handler instead: tap snoozes, hold stops.
    struct GpioKey {
      int8_t pin;
      void (*tap)();
      void (*hold)();
      bool level = true, down = false, longFired = false;
      uint32_t changeAt = 0, downAt = 0;
    };
    GpioKey gkeys[] = {
      { pins::KEY1, [] { renderer::prevPage(); },                  [] { actions::run(actions::Id::BrightDown, "key"); } },
      { pins::KEY2, [] { renderer::nextPage(); },                  [] { actions::run(actions::Id::BrightUp, "key"); } },
      { pins::KEY,  [] { if (!renderer::stepLower(1)) renderer::nextPage(); }, nullptr },   // hold: dismiss (K2)
    };
    bool gpioKeys = false;

#if defined(MWC_BOARD_C3TV)
    bool bothDown = false;                     // Key1 and Key2 held together: power off, not their own actions
    uint32_t bothAt = 0;
    constexpr uint32_t POWER_OFF_MS = 2000;

    // Soft power-off for the battery board: black screen, WiFi off, then light sleep until any key is pressed,
    // which starts the clock afresh. (Deep sleep cannot wake on these pins: the C3 wakes from deep sleep on
    // GPIO 0-5 only.) The backlight is wired on, so it keeps drawing current; only the battery plug cuts it.
    void powerOff() {
      LOGW("power off: both keys held; any key turns it back on");
      panel::blank();
      net_task::pause(true);
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      for (GpioKey& k : gkeys)                             // wait for release, or the held keys wake it at once
        while (k.pin >= 0 && !digitalRead(k.pin)) delay(10);
      delay(100);
      for (GpioKey& k : gkeys) {
        if (k.pin < 0) continue;
        gpio_sleep_set_direction((gpio_num_t)k.pin, GPIO_MODE_INPUT);
        gpio_sleep_set_pull_mode((gpio_num_t)k.pin, GPIO_PULLUP_ONLY);
        gpio_wakeup_enable((gpio_num_t)k.pin, GPIO_INTR_LOW_LEVEL);
      }
      esp_sleep_enable_gpio_wakeup();
      Serial.flush();
      esp_light_sleep_start();
      ESP.restart();
    }
#endif

    void fire(GpioKey& k, bool hold) {
      LOGI("key GPIO %d %s", k.pin, hold ? "hold" : "tap");
      // short K1 = next page / snooze, short K2 = dismiss / stop (a long K2 would mean 'play the chime')
      if (alarmclock::ringing() || (hold && !k.hold)) { if (handler) handler(hold ? K2 : K1, false); return; }
      if (hold) k.hold(); else if (k.tap) k.tap();
    }

    void gpioLoop() {
      const uint32_t now = millis();
      if (now - lastPoll < POLL_MS) return;
      lastPoll = now;
      for (GpioKey& k : gkeys) {
        if (k.pin < 0) continue;
        const bool level = digitalRead(k.pin);                 // active low
        if (level != k.level) { k.level = level; k.changeAt = now; continue; }
        if (now - k.changeAt < DEBOUNCE_MS) continue;
        const bool down = !level;
        if (down && !k.down) { k.down = true; k.longFired = false; k.downAt = now; }
        else if (!down && k.down) { k.down = false; if (!k.longFired) fire(k, false); }
        if (k.down && !k.longFired && now - k.downAt >= LONG_MS) { k.longFired = true; fire(k, true); }
      }
#if defined(MWC_BOARD_C3TV)
      const bool both = gkeys[0].down && gkeys[1].down;
      if (both && !bothDown) { bothDown = true; bothAt = now; }
      if (bothDown) {
        gkeys[0].longFired = gkeys[1].longFired = true;     // no tap or hold of their own once both went down
        if (!gkeys[0].down && !gkeys[1].down) bothDown = false;
        else if (both && now - bothAt >= POWER_OFF_MS) powerOff();
      }
#endif
    }
  }

  void begin(Handler h) {
    handler = h;
    if (pins::KEY >= 0) {
      for (GpioKey& k : gkeys) {
        if (k.pin < 0) continue;
        pinMode(k.pin, INPUT_PULLUP);
        k.level = digitalRead(k.pin);
      }
      gpioKeys = ready = true;
      LOGI("buttons: keys on GPIO %d, %d, %d", pins::KEY1, pins::KEY2, pins::KEY);
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
    if (gpioKeys) { gpioLoop(); return; }
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
