#include "i2c_bus.h"
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "pins.h"
#include "util/log.h"

namespace i2c_bus {
  namespace {
    SemaphoreHandle_t mtx = nullptr;
    Map theMap;
    bool scanned = false;

    bool probeES8311(uint8_t addr) {   // chip ID registers 0xFD = 0x83, 0xFE = 0x11
      uint8_t id1 = 0, id2 = 0;
      Wire.beginTransmission(addr); Wire.write(0xFD);
      if (Wire.endTransmission(false) != 0) return false;
      if (Wire.requestFrom(addr, (uint8_t)1) != 1) return false;
      id1 = Wire.read();
      Wire.beginTransmission(addr); Wire.write(0xFE);
      if (Wire.endTransmission(false) != 0) return false;
      if (Wire.requestFrom(addr, (uint8_t)1) != 1) return false;
      id2 = Wire.read();
      return id1 == 0x83 && id2 == 0x11;
    }
  }

  void begin() {
    if (pins::SDA < 0) { LOGI("I2C: none on this board"); return; }    // no mutex: lock() fails and every access says no
    mtx = xSemaphoreCreateMutex();
    Wire.begin(pins::SDA, pins::SCL, 400000);
    Wire.setTimeOut(50);
  }

  bool lock(uint32_t timeout_ms) { return mtx && xSemaphoreTake(mtx, pdMS_TO_TICKS(timeout_ms)) == pdTRUE; }
  void unlock() { if (mtx) xSemaphoreGive(mtx); }

  namespace {
    volatile bool rescanReq = false;

    // one pass over the bus into m; false when the result looks wrong (a scan started while the driver was busy
    // reports acknowledges at addresses where nothing exists)
    bool scanInto(Map& m) {
      m = Map();
      if (!lock(200)) return false;
      for (uint8_t a = 0x08; a < 0x78 && m.n < sizeof(m.found); a++) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() != 0) continue;
        m.found[m.n++] = a;
        if (a == pins::I2C_ADDR_RTC && pins::HAS_RTC) m.rtc = a;
        else if (a >= 0x18 && a <= 0x1F && pins::HAS_AUDIO) {
          if (!m.es8311 && probeES8311(a)) m.es8311 = a;
          else if (!m.pca9557) m.pca9557 = a;
        }
      }
      unlock();
      return m.n < 12 && (m.rtc != 0 || !pins::HAS_RTC);   // the HUB75 board always has its RTC at 0x51; more than a dozen devices is noise
    }
  }

  const Map& identify() {
    if (scanned) return theMap;
    if (pins::SDA < 0) { scanned = true; return theMap; }   // no bus on this board
    Map m;
    bool ok = scanInto(m);
    if (!ok) { delay(20); ok = scanInto(m); }
    if (ok || !scanned) theMap = m;
    if (!ok) LOGW("I2C scan looked wrong (%u acks, rtc %s); %s", m.n, m.rtc ? "seen" : "missing", scanned ? "kept the previous map" : "using it anyway");
    scanned = true;
    String list;
    for (uint8_t i = 0; i < theMap.n; i++) { char b[8]; snprintf(b, sizeof(b), " 0x%02X", theMap.found[i]); list += b; }
    LOGI("I2C devices:%s | es8311=0x%02X pca9557=0x%02X rtc=0x%02X", list.c_str(), theMap.es8311, theMap.pca9557, theMap.rtc);
    return theMap;
  }

  void requestRescan() { rescanReq = true; }
  void loop() {
    if (!rescanReq) return;
    rescanReq = false;
    Map m;
    bool ok = scanInto(m);
    if (!ok) { delay(20); ok = scanInto(m); }
    if (ok) {
      theMap = m;
      String list;
      for (uint8_t i = 0; i < m.n; i++) { char b[8]; snprintf(b, sizeof(b), " 0x%02X", m.found[i]); list += b; }
      LOGI("I2C rescan:%s", list.c_str());
    } else LOGW("I2C rescan looked wrong (%u acks, rtc %s), kept the previous map", m.n, m.rtc ? "seen" : "missing");
  }
  const Map& map() { return theMap; }

  bool writeReg(uint8_t addr, uint8_t reg, uint8_t val) { return writeRegs(addr, reg, &val, 1); }

  bool readReg(uint8_t addr, uint8_t reg, uint8_t& val) { return readRegs(addr, reg, &val, 1); }

  bool readRegs(uint8_t addr, uint8_t reg, uint8_t* buf, size_t n) {
    if (!lock()) return false;
    bool ok = false;
    Wire.beginTransmission(addr);
    Wire.write(reg);
    if (Wire.endTransmission(false) == 0 && Wire.requestFrom(addr, (uint8_t)n) == n) {
      for (size_t i = 0; i < n; i++) buf[i] = Wire.read();
      ok = true;
    }
    unlock();
    return ok;
  }

  bool writeRegs(uint8_t addr, uint8_t reg, const uint8_t* buf, size_t n) {
    if (!lock()) return false;
    Wire.beginTransmission(addr);
    Wire.write(reg);
    Wire.write(buf, n);
    bool ok = Wire.endTransmission() == 0;
    unlock();
    return ok;
  }
}
