#include "env_sensor.h"
#include <math.h>
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "i2c_bus.h"
#include "util/log.h"

namespace env_sensor {
  namespace {
    constexpr uint8_t ADDRS[2] = { 0x76, 0x77 };
    constexpr uint8_t REG_ID = 0xD0;
    constexpr uint16_t HIST_MIN = 24 * 60;               // one point per minute
    constexpr uint32_t MEAS_WAIT_MS = 80, MEAS_WAIT_GAS_MS = 260;
    constexpr const char* BASELINE_PATH = "/aq_baseline.txt";
    constexpr uint32_t GAS_BURN_IN_MS = 5 * 60000UL;     // heater settles before the score means anything
    constexpr uint32_t AIR_POOR_HOLD_MS = 120000UL;      // poor this long before an alert
    // trend thresholds over the configured windows
    constexpr float TEMP_STEP = 0.5f, TEMP_FAST = 1.5f;          // C
    constexpr float HUM_STEP = 3.0f, HUM_FAST = 8.0f;            // % RH
    constexpr float PRESS_STEP = 1.0f, PRESS_FAST = 3.0f;        // hPa over the pressure window (WMO: 3 h)
    constexpr float AIR_STEP = 10.0f, AIR_FAST = 25.0f;          // score points

    struct Hist { float t, h, p, g, a; };
    IndoorConfig cfg;
    Type kind = Type::None;
    uint8_t addr = 0;
    SemaphoreHandle_t mtx = nullptr;
    Reading cur;
    Hist* hist = nullptr;                                 // PSRAM ring, HIST_MIN entries
    uint16_t histHead = 0, histCount = 0;
    uint32_t lastPoint = 0, lastTrigger = 0, readyAt = 0, errs = 0, startedMs = 0, errRun = 0;
    const char* lastErr = "";
    constexpr uint32_t REINIT_AFTER = 6;   // consecutive failed samples before the sensor is re-initialised
    bool measuring = false, gasOn = false;
    float altHint = NAN, outdoorC = NAN;
    float airBaseline = 0, savedBaseline = 0;             // kOhm
    uint32_t lastBaselineSave = 0, poorSince = 0, lastAirAlert = 0;
    volatile bool airAlertPending = false;

    // ---- BME280 / BMP280 calibration ----
    struct { uint16_t T1; int16_t T2, T3; uint16_t P1; int16_t P2, P3, P4, P5, P6, P7, P8, P9; uint8_t H1; int16_t H2; uint8_t H3; int16_t H4, H5; int8_t H6; } c280;
    // ---- BME680 calibration ----
    struct { uint16_t T1; int16_t T2; int8_t T3; uint16_t P1; int16_t P2; int8_t P3; int16_t P4, P5; int8_t P6, P7; int16_t P8, P9; uint8_t P10;
             uint16_t H1, H2; int8_t H3, H4, H5; uint8_t H6; int8_t H7;
             int8_t GH1; int16_t GH2; int8_t GH3; uint8_t res_heat_range; int8_t res_heat_val; int8_t range_sw_err; } c680;
    uint8_t variant680 = 0;   // register 0xF0: 0 = BME680, 1 = BME688 (gas result in other registers, other resistance formula)

    bool take() { return mtx && xSemaphoreTake(mtx, pdMS_TO_TICKS(20)) == pdTRUE; }
    void give() { xSemaphoreGive(mtx); }
    inline uint16_t u16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
    inline int16_t s16(const uint8_t* p) { return (int16_t)u16(p); }

    bool init280() {
      uint8_t c[26], h[7];
      if (!i2c_bus::readRegs(addr, 0x88, c, 26)) return false;
      c280.T1 = u16(c); c280.T2 = s16(c + 2); c280.T3 = s16(c + 4);
      c280.P1 = u16(c + 6); c280.P2 = s16(c + 8); c280.P3 = s16(c + 10); c280.P4 = s16(c + 12); c280.P5 = s16(c + 14);
      c280.P6 = s16(c + 16); c280.P7 = s16(c + 18); c280.P8 = s16(c + 20); c280.P9 = s16(c + 22);
      c280.H1 = c[25];
      if (kind == Type::BME280) {
        if (!i2c_bus::readRegs(addr, 0xE1, h, 7)) return false;
        c280.H2 = s16(h); c280.H3 = h[2];
        c280.H4 = (int16_t)((h[3] << 4) | (h[4] & 0x0F));
        c280.H5 = (int16_t)((h[5] << 4) | (h[4] >> 4));
        c280.H6 = (int8_t)h[6];
        i2c_bus::writeReg(addr, 0xF2, 0x01);           // humidity oversampling x1
      }
      return i2c_bus::writeReg(addr, 0xF5, 0x00);      // filter off, sleep between forced measurements
    }

    // heater resistance register value for a target plate temperature (BME680 datasheet, float version)
    uint8_t heaterCode(float targetC, float ambC) {
      float var1 = ((float)c680.GH1 / 16.0f) + 49.0f;
      float var2 = (((float)c680.GH2 / 32768.0f) * 0.0005f) + 0.00235f;
      float var3 = (float)c680.GH3 / 1024.0f;
      float var4 = var1 * (1.0f + (var2 * targetC));
      float var5 = var4 + (var3 * ambC);
      float r = 3.4f * ((var5 * (4.0f / (4.0f + (float)c680.res_heat_range)) * (1.0f / (1.0f + ((float)c680.res_heat_val * 0.002f)))) - 25.0f);
      if (r < 0) r = 0; if (r > 255) r = 255;
      return (uint8_t)r;
    }

    bool init680() {
      uint8_t a[23], b[14], c[5];
      if (!i2c_bus::readRegs(addr, 0x8A, a, 23) || !i2c_bus::readRegs(addr, 0xE1, b, 14) || !i2c_bus::readRegs(addr, 0x00, c, 5)) return false;
      c680.T2 = s16(a); c680.T3 = (int8_t)a[2];
      c680.P1 = u16(a + 4); c680.P2 = s16(a + 6); c680.P3 = (int8_t)a[8]; c680.P4 = s16(a + 10); c680.P5 = s16(a + 12);
      c680.P7 = (int8_t)a[14]; c680.P6 = (int8_t)a[15]; c680.P8 = s16(a + 18); c680.P9 = s16(a + 20); c680.P10 = a[22];   // 0x9C/0x9D, 0x9E/0x9F, 0xA0
      c680.H2 = (uint16_t)((b[0] << 4) | (b[1] >> 4));
      c680.H1 = (uint16_t)((b[2] << 4) | (b[1] & 0x0F));
      c680.H3 = (int8_t)b[3]; c680.H4 = (int8_t)b[4]; c680.H5 = (int8_t)b[5]; c680.H6 = b[6]; c680.H7 = (int8_t)b[7];
      c680.T1 = u16(b + 8);
      c680.GH2 = s16(b + 10); c680.GH1 = (int8_t)b[12]; c680.GH3 = (int8_t)b[13];
      if (!i2c_bus::readReg(addr, 0xF0, variant680)) variant680 = 0;
      variant680 &= 1;
      c680.res_heat_val = (int8_t)c[0];
      c680.res_heat_range = (uint8_t)((c[2] >> 4) & 0x03);
      c680.range_sw_err = (int8_t)((int8_t)(c[4] & 0xF0) / 16);
      bool ok = i2c_bus::writeReg(addr, 0x72, 0x01);   // humidity oversampling x1
      ok &= i2c_bus::writeReg(addr, 0x75, 0x00);       // filter off
      gasOn = cfg.gas;
      if (gasOn) {
        const uint8_t hc = heaterCode(320.0f, 25.0f);
        LOGI("indoor: %s heater code %u for 320 C (GH1 %d GH2 %d GH3 %d range %u val %d sw_err %d)", variant680 ? "BME688" : "BME680", hc, c680.GH1, c680.GH2, c680.GH3, c680.res_heat_range, c680.res_heat_val, c680.range_sw_err);
        ok &= i2c_bus::writeReg(addr, 0x5A, hc);                            // heater profile 0: 320 C
        ok &= i2c_bus::writeReg(addr, 0x64, 0x65);     // gas_wait_0: 37 x 4 ms = 148 ms
        ok &= i2c_bus::writeReg(addr, 0x70, 0x00);     // heater on
        ok &= i2c_bus::writeReg(addr, 0x71, variant680 ? 0x20 : 0x10);   // run_gas is bit 4 on the BME680, bit 5 on the BME688; profile 0
      } else {
        ok &= i2c_bus::writeReg(addr, 0x70, 0x08);     // heater off
        ok &= i2c_bus::writeReg(addr, 0x71, 0x00);     // gas measurement off
      }
      return ok;
    }

    bool trigger() {
      // temperature x2, pressure x4, forced mode
      return i2c_bus::writeReg(addr, kind == Type::BME680 ? 0x74 : 0xF4, (2 << 5) | (4 << 2) | 1);
    }

    bool read280(float& t, float& p, float& h) {
      uint8_t d[8];
      if (!i2c_bus::readRegs(addr, 0xF7, d, 8)) return false;
      int32_t adcP = ((int32_t)d[0] << 12) | ((int32_t)d[1] << 4) | (d[2] >> 4);
      int32_t adcT = ((int32_t)d[3] << 12) | ((int32_t)d[4] << 4) | (d[5] >> 4);
      int32_t adcH = ((int32_t)d[6] << 8) | d[7];
      if (adcT == 0x80000 || adcP == 0x80000) return false;
      int32_t var1 = ((((adcT >> 3) - ((int32_t)c280.T1 << 1))) * ((int32_t)c280.T2)) >> 11;
      int32_t var2 = (((((adcT >> 4) - ((int32_t)c280.T1)) * ((adcT >> 4) - ((int32_t)c280.T1))) >> 12) * ((int32_t)c280.T3)) >> 14;
      int32_t t_fine = var1 + var2;
      t = (float)((t_fine * 5 + 128) >> 8) / 100.0f;
      int64_t v1 = (int64_t)t_fine - 128000;
      int64_t v2 = v1 * v1 * (int64_t)c280.P6;
      v2 = v2 + ((v1 * (int64_t)c280.P5) << 17);
      v2 = v2 + (((int64_t)c280.P4) << 35);
      v1 = ((v1 * v1 * (int64_t)c280.P3) >> 8) + ((v1 * (int64_t)c280.P2) << 12);
      v1 = (((((int64_t)1) << 47) + v1)) * ((int64_t)c280.P1) >> 33;
      if (v1 == 0) return false;
      int64_t pp = 1048576 - adcP;
      pp = (((pp << 31) - v2) * 3125) / v1;
      v1 = (((int64_t)c280.P9) * (pp >> 13) * (pp >> 13)) >> 25;
      v2 = (((int64_t)c280.P8) * pp) >> 19;
      pp = ((pp + v1 + v2) >> 8) + (((int64_t)c280.P7) << 4);
      p = (float)pp / 256.0f / 100.0f;                  // Pa -> hPa
      h = 0;
      if (kind == Type::BME280 && adcH != 0x8000) {
        int32_t vx = t_fine - 76800;
        vx = (((((adcH << 14) - (((int32_t)c280.H4) << 20) - (((int32_t)c280.H5) * vx)) + 16384) >> 15) *
              (((((((vx * ((int32_t)c280.H6)) >> 10) * (((vx * ((int32_t)c280.H3)) >> 11) + 32768)) >> 10) + 2097152) * ((int32_t)c280.H2) + 8192) >> 14));
        vx = vx - (((((vx >> 15) * (vx >> 15)) >> 7) * ((int32_t)c280.H1)) >> 4);
        if (vx < 0) vx = 0;
        if (vx > 419430400) vx = 419430400;
        h = (float)(vx >> 12) / 1024.0f;
      }
      return true;
    }

    // BME680 gas resistance from the raw ADC value and range (datasheet float version)
    float gasResistance(uint16_t adc, uint8_t range) {
      static const float K1[16] = { 1, 1, 1, 1, 1, 0.99f, 1, 0.992f, 1, 1, 0.998f, 0.995f, 1, 0.99f, 1, 1 };
      static const float K2[16] = { 8000000, 4000000, 2000000, 1000000, 499500.4f, 250000, 125000, 62500, 31250, 15625, 7812.5f, 3906.25f, 1953.125f, 976.5625f, 488.28125f, 244.140625f };
      float var1 = (1340.0f + (5.0f * (float)c680.range_sw_err)) * K1[range & 15];
      return var1 * K2[range & 15] / ((float)adc - 512.0f + var1);
    }

    bool read680(float& t, float& p, float& h, bool& gasValid, float& gasOhm) {
      uint8_t d[17];
      if (!i2c_bus::readRegs(addr, 0x1D, d, 17)) return false;
      if (!(d[0] & 0x80)) return false;                 // new_data not set yet
      float adcP = (float)(((uint32_t)d[2] << 12) | ((uint32_t)d[3] << 4) | (d[4] >> 4));
      float adcT = (float)(((uint32_t)d[5] << 12) | ((uint32_t)d[6] << 4) | (d[7] >> 4));
      float adcH = (float)(((uint32_t)d[8] << 8) | d[9]);
      float var1 = ((adcT / 16384.0f) - ((float)c680.T1 / 1024.0f)) * (float)c680.T2;
      float var2 = (((adcT / 131072.0f) - ((float)c680.T1 / 8192.0f)) * ((adcT / 131072.0f) - ((float)c680.T1 / 8192.0f))) * ((float)c680.T3 * 16.0f);
      float t_fine = var1 + var2;
      t = t_fine / 5120.0f;
      var1 = (t_fine / 2.0f) - 64000.0f;
      var2 = var1 * var1 * ((float)c680.P6 / 131072.0f);
      var2 = var2 + (var1 * (float)c680.P5 * 2.0f);
      var2 = (var2 / 4.0f) + ((float)c680.P4 * 65536.0f);
      var1 = ((((float)c680.P3 * var1 * var1) / 16384.0f) + ((float)c680.P2 * var1)) / 524288.0f;
      var1 = (1.0f + (var1 / 32768.0f)) * (float)c680.P1;
      float pc = 1048576.0f - adcP;
      if (var1 == 0) return false;
      pc = ((pc - (var2 / 4096.0f)) * 6250.0f) / var1;
      var1 = ((float)c680.P9 * pc * pc) / 2147483648.0f;
      var2 = pc * ((float)c680.P8 / 32768.0f);
      float var3 = (pc / 256.0f) * (pc / 256.0f) * (pc / 256.0f) * ((float)c680.P10 / 131072.0f);
      pc = pc + (var1 + var2 + var3 + ((float)c680.P7 * 128.0f)) / 16.0f;
      p = pc / 100.0f;
      float tc = t;
      var1 = adcH - (((float)c680.H1 * 16.0f) + (((float)c680.H3 / 2.0f) * tc));
      var2 = var1 * (((float)c680.H2 / 262144.0f) * (1.0f + (((float)c680.H4 / 16384.0f) * tc) + (((float)c680.H5 / 1048576.0f) * tc * tc)));
      var3 = (float)c680.H6 / 16384.0f;
      float var4 = (float)c680.H7 / 2097152.0f;
      h = var2 + ((var3 + (var4 * tc)) * var2 * var2);
      if (h > 100) h = 100; else if (h < 0) h = 0;
      gasValid = false; gasOhm = 0;
      if (gasOn) {
        // BME680: gas_r at 0x2A/0x2B (d[13], d[14]); BME688: at 0x2C/0x2D (d[15], d[16]). The lsb carries gas_valid (bit 5),
        // heat_stab (bit 4) and the range (bits 3:0).
        const uint8_t msb = variant680 ? d[15] : d[13], lsb = variant680 ? d[16] : d[14];
        const uint16_t adcG = (uint16_t)(((uint16_t)msb << 2) | (lsb >> 6));
        if ((lsb & 0x20) && (lsb & 0x10)) {
          gasValid = true;
          if (variant680) {   // BME688 formula (bme68x driver, "high" variant)
            const uint32_t var1 = 262144u >> (lsb & 0x0F);
            const int32_t var2 = 4096 + ((int32_t)adcG - 512) * 3;
            gasOhm = var2 > 0 ? (1000000.0f * (float)var1) / (float)var2 : 0;
          } else gasOhm = gasResistance(adcG, lsb & 0x0F);
        }
      }
      return true;
    }

    float altitude() { return cfg.altitude_m >= 0 ? cfg.altitude_m : altHint; }

    Trend classify(float d, float step, float fast) {
      if (d >= fast) return Trend::RisingFast;
      if (d >= step) return Trend::Rising;
      if (d <= -fast) return Trend::FallingFast;
      if (d <= -step) return Trend::Falling;
      return Trend::Steady;
    }

    uint16_t lookBack(uint16_t back, Hist& out) {   // value `back` minutes ago or the oldest point; returns minutes spanned
      if (!histCount) return 0;
      uint16_t n = back < histCount ? back : histCount;
      uint16_t idx = (uint16_t)((histHead + HIST_MIN - n) % HIST_MIN);
      out = hist[idx];
      return n;
    }

    void updateTrends(Reading& r) {
      Hist old;
      r.span_min = 0;
      uint16_t n = lookBack(cfg.trend_min, old);
      if (n >= 10) {
        r.d_temp = r.temp_c - old.t;
        r.d_hum = r.humidity - old.h;
        r.d_air = r.air_score - old.a;
        r.t_temp = classify(r.d_temp, TEMP_STEP, TEMP_FAST);
        r.t_hum = r.has_humidity ? classify(r.d_hum, HUM_STEP, HUM_FAST) : Trend::Steady;
        r.t_air = (r.air_ready && old.a > 0) ? classify(r.d_air, AIR_STEP, AIR_FAST) : Trend::Steady;
        r.span_min = n;
      } else { r.d_temp = r.d_hum = r.d_air = 0; r.t_temp = r.t_hum = r.t_air = Trend::Steady; }
      n = lookBack(cfg.pressure_trend_min, old);
      if (n >= 30) {
        r.d_press = (r.pressure_hpa - old.p) * (float)cfg.pressure_trend_min / (float)n;   // scaled to the full window
        r.t_press = classify(r.d_press, PRESS_STEP, PRESS_FAST);
        if (n > r.span_min) r.span_min = n;
      } else { r.d_press = 0; r.t_press = Trend::Steady; }
    }

    uint8_t mouldRisk() {   // humidity history: >= 70 % for 2 h = high, >= 60 % for 6 h = elevated
      if (histCount < 120) return 0;
      bool high = true, elevated = histCount >= 360;
      for (uint16_t k = 1; k <= 360 && k <= histCount; k++) {
        const Hist& h = hist[(histHead + HIST_MIN - k) % HIST_MIN];
        if (k <= 120 && h.h < 70) high = false;
        if (h.h < 60) { elevated = false; if (!high || k > 120) break; }
      }
      return high ? 2 : (elevated ? 1 : 0);
    }

    void derive(Reading& r) {
      const float T = r.temp_c, RH = r.has_humidity ? r.humidity : 0;
      if (r.has_humidity && RH > 0.5f) {
        const float a = 17.62f, b = 243.12f;
        float gamma = logf(RH / 100.0f) + a * T / (b + T);
        r.dew_point_c = b * gamma / (a - gamma);
        r.abs_humidity = 216.7f * ((RH / 100.0f) * 6.112f * expf(17.62f * T / (243.12f + T))) / (273.15f + T);
        if (!isnan(outdoorC)) r.condensation = outdoorC <= r.dew_point_c ? 2 : (outdoorC <= r.dew_point_c + 3.0f ? 1 : 0);
        else r.condensation = 0;
        r.mould_risk = mouldRisk();
        if (T >= 26.7f && RH >= 40.0f) {   // Rothfusz heat index (computed in F)
          float tf = T * 9.0f / 5.0f + 32.0f;
          float hi = -42.379f + 2.04901523f * tf + 10.14333127f * RH - 0.22475541f * tf * RH - 0.00683783f * tf * tf
                     - 0.05481717f * RH * RH + 0.00122874f * tf * tf * RH + 0.00085282f * tf * RH * RH - 0.00000199f * tf * tf * RH * RH;
          r.heat_index_c = (hi - 32.0f) * 5.0f / 9.0f;
        } else r.heat_index_c = T;
      } else { r.dew_point_c = NAN; r.abs_humidity = 0; r.heat_index_c = T; }
    }

    void saveBaseline(uint32_t now, bool force) {
      if (airBaseline <= 0) return;
      const bool changed = fabsf(airBaseline - savedBaseline) > savedBaseline * 0.05f;
      if (!force && (!changed || now - lastBaselineSave < 30 * 60000UL)) return;
      File f = LittleFS.open(BASELINE_PATH, "w");
      if (!f) return;
      f.printf("%.3f\n", airBaseline);
      f.close();
      savedBaseline = airBaseline;
      lastBaselineSave = now;
    }

    void loadBaseline() {
      File f = LittleFS.open(BASELINE_PATH, "r");
      if (!f) return;
      float v = f.parseFloat();
      f.close();
      if (v > 0.5f && v < 2000.0f) { airBaseline = savedBaseline = v; LOGI("indoor: air baseline %.1f kOhm restored", v); }
      else LOGW("indoor: stored air baseline %.1f kOhm is not plausible, starting over", v);
    }

    // relative air quality: 75 % from gas resistance against the learned clean-air baseline, 25 % humidity comfort
    void updateAir(Reading& r, bool gasValid, float gasOhm, uint32_t now) {
      r.has_gas = gasOn;
      r.gas_valid = gasValid;
      r.gas_kohm = gasValid ? gasOhm / 1000.0f : 0;
      r.air_baseline_kohm = airBaseline;
      r.air_ready = false;
      r.air_level = AIR_UNKNOWN;
      r.air_score = 0;
      if (!gasOn || !gasValid) return;
      if (now - startedMs < GAS_BURN_IN_MS) return;                       // heater still settling
      const float g = r.gas_kohm;
      if (g <= 0.5f || g > 2000.0f) return;                                // not a real heated-plate reading
      if (g > airBaseline) airBaseline = g;                                // clean air raises the baseline at once
      else airBaseline -= (airBaseline - g) * 0.00006f;                    // and it drifts down slowly (~2 % per hour at 10 s samples)
      r.air_baseline_kohm = airBaseline;
      if (airBaseline <= 0.5f) return;
      float gasScore = 75.0f * (g / airBaseline);
      if (gasScore > 75.0f) gasScore = 75.0f;
      float humScore = r.has_humidity ? 25.0f * (1.0f - fabsf(r.humidity - 40.0f) / 40.0f) : 25.0f;
      if (humScore < 0) humScore = 0;
      r.air_score = gasScore + humScore;
      r.air_ready = true;
      r.air_level = r.air_score >= cfg.air_fair_below ? AIR_GOOD : (r.air_score >= cfg.air_poor_below ? AIR_FAIR : AIR_POOR);
      saveBaseline(now, false);
      // alert after the air has been poor for a while, at most every air_alert_min minutes
      if (r.air_level == AIR_POOR) {
        if (!poorSince) poorSince = now;
        else if (cfg.air_alert && now - poorSince >= AIR_POOR_HOLD_MS && (lastAirAlert == 0 || now - lastAirAlert >= (uint32_t)cfg.air_alert_min * 60000UL)) {
          lastAirAlert = now;
          airAlertPending = true;
        }
      } else poorSince = 0;
    }

    void publish(float t, float p, float h, bool gasValid, float gasOhm, uint32_t now) {
      Reading r;
      r.valid = true;
      r.has_humidity = kind != Type::BMP280;
      r.temp_c = t + cfg.temp_offset_c;
      r.humidity = r.has_humidity ? constrain(h + cfg.humidity_offset, 0.0f, 100.0f) : 0;
      r.pressure_hpa = p;
      float alt = altitude();
      if (!isnan(alt)) { r.altitude_m = alt; r.sea_level_known = true; r.sea_level_hpa = p / powf(1.0f - alt / 44330.0f, 5.255f); }
      else { r.sea_level_hpa = p; r.sea_level_known = false; r.altitude_m = 0; }
      r.sample_ms = now;
      updateAir(r, gasValid, gasOhm, now);
      if (hist && (lastPoint == 0 || now - lastPoint >= 60000UL)) {
        lastPoint = now;
        hist[histHead] = { r.temp_c, r.humidity, r.pressure_hpa, r.gas_kohm, r.air_ready ? r.air_score : 0 };
        histHead = (uint16_t)((histHead + 1) % HIST_MIN);
        if (histCount < HIST_MIN) histCount++;
      }
      updateTrends(r);
      derive(r);
      if (take()) { cur = r; give(); }
    }
  }

  namespace {
    uint32_t lastProbe = 0;
    volatile bool detectedEvent = false, probeReq = false;
    constexpr uint32_t PROBE_MS = 30000;   // while nothing is connected, look again this often (hot plug)

    bool probe(bool loud) {
      for (uint8_t a : ADDRS) {
        uint8_t id = 0;
        if (!i2c_bus::readReg(a, REG_ID, id)) continue;
        if (id == 0x60) kind = Type::BME280;
        else if (id == 0x58) kind = Type::BMP280;
        else if (id == 0x61) kind = Type::BME680;
        else continue;
        addr = a;
        break;
      }
      if (kind == Type::None) { if (loud) LOGI("indoor: no BME280/BMP280/BME680 found at 0x76/0x77"); return false; }
      bool ok = kind == Type::BME680 ? init680() : init280();
      if (!ok) { LOGW("indoor: %s at 0x%02X did not answer during setup", typeName(), addr); kind = Type::None; addr = 0; return false; }
      if (!hist) hist = (Hist*)heap_caps_malloc(sizeof(Hist) * HIST_MIN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (!hist) hist = (Hist*)malloc(sizeof(Hist) * HIST_MIN);
      histHead = histCount = 0; lastPoint = 0; measuring = false; lastTrigger = 0; errs = 0;
      startedMs = millis();
      if (gasOn) loadBaseline();
      LOGI("indoor: %s at 0x%02X%s", typeName(), addr, gasOn ? ", gas sensor on" : "");
      return true;
    }
  }

  void begin(const IndoorConfig& ic) {
    cfg = ic;
    if (!mtx) mtx = xSemaphoreCreateMutex();
    if (!cfg.enabled) { LOGI("indoor: disabled"); return; }
    probe(true);
    lastProbe = millis();
  }

  void requestRescan() { probeReq = true; }

  bool consumeDetectedEvent() { if (!detectedEvent) return false; detectedEvent = false; return true; }

  void apply(const IndoorConfig& ic) {
    const bool was = cfg.enabled, gasWas = cfg.gas;
    cfg = ic;
    if (cfg.enabled && !was && kind == Type::None) { begin(ic); return; }
    if (!cfg.enabled) { if (take()) { cur = Reading(); give(); } return; }
    if (kind == Type::BME680 && gasWas != cfg.gas) { init680(); startedMs = millis(); poorSince = 0; if (gasOn) loadBaseline(); }
  }

  void loop(uint32_t now) {
    if (!cfg.enabled) return;
    if (kind == Type::None) {                       // nothing connected: look again now and then, or when asked
      const bool asked = probeReq;
      if (asked || now - lastProbe >= PROBE_MS) { probeReq = false; lastProbe = now; if (probe(asked)) detectedEvent = true; }
      return;
    }
    probeReq = false;
    if (!measuring) {
      uint32_t period = (uint32_t)(cfg.sample_sec ? cfg.sample_sec : 10) * 1000UL;
      if (lastTrigger && now - lastTrigger < period) return;
      lastTrigger = now;
      if (trigger()) { measuring = true; readyAt = now + (gasOn ? MEAS_WAIT_GAS_MS : MEAS_WAIT_MS); }
      else errs++;
      return;
    }
    if ((int32_t)(now - readyAt) < 0) return;
    measuring = false;
    float t, p, h, gasOhm = 0;
    bool gasValid = false;
    bool ok = kind == Type::BME680 ? read680(t, p, h, gasValid, gasOhm) : read280(t, p, h);
    if (!ok || t < -45 || t > 90 || p < 300 || p > 1200) {
      errs++; errRun++;
      lastErr = !ok ? (i2c_bus::readReg(addr, REG_ID, *(uint8_t*)&t) ? "not ready" : "i2c") : "out of range";
      if (errRun == REINIT_AFTER) {
        // the sensor stopped answering or lost its settings (loose cable, power dip, chip reset): start over. If it is
        // gone from the bus the hot-plug probe finds it again when it comes back.
        LOGW("indoor: %lu failed samples in a row (%s), re-initialising %s", (unsigned long)errRun, lastErr, typeName());
        Type was = kind;
        kind = Type::None; addr = 0;
        if (take()) { cur = Reading(); give(); }
        lastProbe = 0;
        if (probe(false)) { errRun = 0; LOGI("indoor: %s back", typeName()); }
        else LOGW("indoor: %s not answering, waiting for it to come back", was == Type::BME680 ? "BME680" : "sensor");
      }
      return;
    }
    if (errRun) LOGI("indoor: reading again after %lu failed samples", (unsigned long)errRun);
    errRun = 0; lastErr = "";
    publish(t, p, h, gasValid, gasOhm, now);
  }

  void setAltitudeHint(float meters) { altHint = meters; }
  void setOutdoorTempC(float c) { outdoorC = c; }
  Type type() { return kind; }
  const char* typeName() {
    switch (kind) {
      case Type::BMP280: return "BMP280";
      case Type::BME280: return "BME280";
      case Type::BME680: return variant680 ? "BME688" : "BME680";
      default: return "none";
    }
  }
  uint8_t address() { return addr; }
  bool present() { return kind != Type::None && cfg.enabled; }
  bool hasGas() { return present() && gasOn; }
  Reading reading() { Reading r; if (take()) { r = cur; give(); } return r; }

  size_t history(HistoryPoint* out, size_t max, uint16_t minutes, uint16_t step_min) {
    if (!hist || !histCount || !max) return 0;
    if (!step_min) step_min = 1;
    uint16_t span = minutes < histCount ? minutes : histCount;
    size_t n = 0;
    for (int32_t back = span; back >= 1 && n < max; back -= step_min) {
      const Hist& h = hist[(histHead + HIST_MIN - back) % HIST_MIN];
      out[n++] = { (uint16_t)(back - 1), h.t, h.h, h.p, h.g, h.a };
    }
    return n;
  }

  bool consumeAirAlert() { if (!airAlertPending) return false; airAlertPending = false; return true; }

  const char* trendName(Trend t) {
    switch (t) {
      case Trend::RisingFast: return "rising fast";
      case Trend::Rising: return "rising";
      case Trend::Falling: return "falling";
      case Trend::FallingFast: return "falling fast";
      default: return "steady";
    }
  }
  const char* airLevelName(uint8_t l) { return l == AIR_GOOD ? "good" : l == AIR_FAIR ? "fair" : l == AIR_POOR ? "poor" : "unknown"; }
  const char* condensationName(uint8_t c) { return c == 2 ? "likely" : c == 1 ? "possible" : "none"; }
  const char* mouldName(uint8_t m) { return m == 2 ? "high" : m == 1 ? "elevated" : "none"; }
  uint32_t errors() { return errs; }
  const char* lastError() { return lastErr; }
  uint32_t consecutiveErrors() { return errRun; }
}
