#include "touch.h"
#include <Wire.h>
#include "i2c_bus.h"
#include "actions.h"
#include "pins.h"
#include "alarm/alarm.h"
#include "config/config.h"
#include "display/renderer.h"
#include "util/log.h"

namespace touch {
  namespace {
    constexpr uint16_t REG_PRODUCT_ID = 0x8140;   // "911"
    constexpr uint16_t REG_STATUS = 0x814E;       // bit 7: new data ready, bits 0-3: touch points
    constexpr uint16_t REG_POINT1 = 0x814F;       // track id, x (LE16), y (LE16), size (LE16), reserved
    constexpr uint32_t POLL_MS = 20, LONG_MS = 1500, LOST_MS = 500;
    constexpr int16_t SCREEN = 480;
    constexpr int16_t SWIPE_PX = 70;              // shortest travel that counts as a swipe
    constexpr int16_t STILL_PX = 30;              // a hold must stay within this distance to count as a long press

    buttons::Handler handler = nullptr;
    uint8_t addr = 0;
    bool touched = false, longFired = false, moved = false;
    uint32_t lastPoll = 0, downAt = 0, lastReport = 0;
    int16_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;      // first and latest point of the current touch, screen pixels

    bool readReg16(uint8_t a, uint16_t reg, uint8_t* buf, uint8_t n) {
      if (!i2c_bus::lock()) return false;
      Wire.beginTransmission(a);
      Wire.write((uint8_t)(reg >> 8));
      Wire.write((uint8_t)reg);
      bool ok = Wire.endTransmission(false) == 0 && Wire.requestFrom(a, n) == n;
      if (ok) for (uint8_t i = 0; i < n; i++) buf[i] = Wire.read();
      i2c_bus::unlock();
      return ok;
    }

    bool writeReg16(uint8_t a, uint16_t reg, uint8_t v) {
      if (!i2c_bus::lock()) return false;
      Wire.beginTransmission(a);
      Wire.write((uint8_t)(reg >> 8));
      Wire.write((uint8_t)reg);
      Wire.write(v);
      bool ok = Wire.endTransmission() == 0;
      i2c_bus::unlock();
      return ok;
    }

    // Turns a vector on the glass into the picture's own frame. The panel draws the picture turned clockwise by
    // `rotation` quarter turns (logical right appears as screen down at 90 degrees), so undo that here.
    void toPicture(int16_t sx, int16_t sy, int16_t& lx, int16_t& ly) {
      lx = sx; ly = sy;
      for (uint8_t r = 0; r < (g_cfg.panel.rotation & 3); r++) { const int16_t t = lx; lx = ly; ly = -t; }
    }

    void swipe(int16_t dx, int16_t dy) {
      if (alarmclock::ringing()) { if (handler) handler(buttons::K1, false); return; }   // snooze, as a tap would
      int16_t lx, ly, cx, cy;
      toPicture(dx, dy, lx, ly);
      toPicture((int16_t)(x0 - SCREEN / 2), (int16_t)(y0 - SCREEN / 2), cx, cy);       // where it started, from the centre
      const bool bottom = cy > 0;
      if (abs(lx) >= abs(ly)) {
        const int8_t dir = lx < 0 ? 1 : -1;      // a swipe to the left brings in the next one
        LOGI("touch: swipe %s on the %s half", dir > 0 ? "left" : "right", bottom ? "bottom" : "top");
        if (bottom && renderer::stepLower(dir)) return;
        if (dir > 0) renderer::nextPage(); else renderer::prevPage();
      } else {
        LOGI("touch: swipe %s", ly < 0 ? "up" : "down");
        actions::run(ly < 0 ? actions::Id::BrightUp : actions::Id::BrightDown, "touch");
      }
    }

    void release() {
      touched = false;
      const int16_t dx = x1 - x0, dy = y1 - y0;
      if (max(abs(dx), abs(dy)) >= SWIPE_PX) swipe(dx, dy);
      else if (!longFired && handler) handler(buttons::K1, false);
    }
  }

  bool begin(buttons::Handler h) {
    handler = h;
    if (!pins::HAS_TOUCH) return false;
    for (uint8_t a : { (uint8_t)0x5D, (uint8_t)0x14 }) {
      uint8_t id[4] = {0};
      if (readReg16(a, REG_PRODUCT_ID, id, 4) && id[0] == '9') { addr = a; break; }
    }
    if (!addr) { LOGW("touch: no GT911 found"); return false; }
    writeReg16(addr, REG_STATUS, 0);
    LOGI("touch: GT911 at 0x%02X", addr);
    return true;
  }

  bool available() { return addr != 0; }

  void loop() {
    if (!addr) return;
    const uint32_t now = millis();
    if (now - lastPoll < POLL_MS) return;
    lastPoll = now;
    uint8_t st;
    if (!readReg16(addr, REG_STATUS, &st, 1)) return;
    if (st & 0x80) {
      uint8_t p[8];
      const bool down = (st & 0x0F) != 0 && readReg16(addr, REG_POINT1, p, sizeof(p));
      writeReg16(addr, REG_STATUS, 0);        // hand the buffer back to the controller
      lastReport = now;
      if (down) {
        const int16_t x = (int16_t)(p[1] | (p[2] << 8)), y = (int16_t)(p[3] | (p[4] << 8));
        if (!touched) { touched = true; longFired = false; moved = false; downAt = now; x0 = x1 = x; y0 = y1 = y; }
        else { x1 = x; y1 = y; }
        if (max(abs(x1 - x0), abs(y1 - y0)) > STILL_PX) moved = true;
      } else if (touched) release();
    } else if (touched && now - lastReport > LOST_MS) {
      release();                               // a lift that was never reported
    }
    if (touched && !longFired && !moved && now - downAt >= LONG_MS) {
      longFired = true;
      if (handler) handler(buttons::K2, false);
    }
  }
}
