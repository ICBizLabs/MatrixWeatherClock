#include "touch.h"
#include <Wire.h>
#include "i2c_bus.h"
#include "pins.h"
#include "util/log.h"

namespace touch {
  namespace {
    constexpr uint16_t REG_PRODUCT_ID = 0x8140;   // "911"
    constexpr uint16_t REG_STATUS = 0x814E;       // bit 7: new data ready, bits 0-3: touch points
    constexpr uint32_t POLL_MS = 25, LONG_MS = 1500, LOST_MS = 500;

    buttons::Handler handler = nullptr;
    uint8_t addr = 0;
    bool touched = false, longFired = false;
    uint32_t lastPoll = 0, downAt = 0, lastReport = 0;

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

    void release() {
      touched = false;
      if (!longFired && handler) handler(buttons::K1, false);
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
      writeReg16(addr, REG_STATUS, 0);        // hand the buffer back to the controller
      lastReport = now;
      const bool down = (st & 0x0F) != 0;
      if (down && !touched) { touched = true; longFired = false; downAt = now; }
      else if (!down && touched) release();
    } else if (touched && now - lastReport > LOST_MS) {
      release();                            // a lift that was never reported
    }
    if (touched && !longFired && now - downAt >= LONG_MS) {
      longFired = true;
      if (handler) handler(buttons::K2, false);
    }
  }
}
