#include "renderer.h"
#include "app.h"
#include <Fonts/TomThumb.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <time.h>
#include "layout.h"
#include "panel.h"
#include "scroller.h"
#include "wmo.h"
#include "themes.h"
#include "test_pattern.h"
#include "fonts/digits_7x11.h"
#include "config/config.h"
#include "net/shared_state.h"
#include "net/alert_store.h"
#include "net/wifi_manager.h"
#include "net/lightning.h"
#include "io/env_sensor.h"
#include "util/zambretti.h"
#include "util/log.h"
#include "net/radar.h"
#include "net/tide.h"
#include "util/moon.h"
#include <esp_heap_caps.h>
#include "time/time_service.h"
#include "alarm/alarm.h"
#include "util/timeutil.h"
#include "version.h"

namespace renderer {
  namespace {
    using namespace layout;
    enum class Screen : uint8_t { Splash, Composite, Forecast, Hourly, Radar, Test };
    Screen screen = Screen::Splash;
    uint32_t screenUntil = 0, testStart = 0;
    bool otaActive = false;
    uint8_t otaPct = 0;
    uint32_t ipUntil = 0;

    // bottom-half page canvases (local coordinates, 64x16) for slide transitions
    Canvas* pageA = nullptr;
    Canvas* pageB = nullptr;
    constexpr int16_t Y_L1 = LINE1_Y - BOTTOM_Y, Y_L2 = LINE2_Y - BOTTOM_Y, Y_SINGLE = SINGLE_Y - BOTTOM_Y, Y_BANNER = BANNER_Y - BOTTOM_Y;
    constexpr uint32_t TRANS_MS = 320;
    uint8_t pageIdx = 0;
    uint32_t pageSince = 0;
    uint8_t transFrom = 255;       // page id sliding out, 255 = no transition running
    uint32_t transStart = 0;
    bool transFromLightning = false, showLightningPage = false, lightningTurn = false;
    int8_t briOffset = 0;                       // remote brightness steps (x16)
    uint8_t nightOvr = 0;                       // 0 auto, 1 on, 2 off
    uint8_t cyclesSinceFull = 0, cyclesSinceRadar = 0;
    uint8_t fullTurn = 0;                       // round-robin over the enabled full screens
    uint16_t* radarFrame = nullptr;             // PSRAM, W*H pixels (allocated in begin)
    uint8_t* radarBase = nullptr;               // PSRAM, W*H mask bytes
    constexpr size_t RADAR_PX = (size_t)radar::W * radar::H;
    bool radarBaseOk = false;
    uint8_t radarIdx = 0;
    uint32_t radarNextAt = 0;
    bool radarLoaded = false;
    uint16_t* demoRadar = nullptr;              // demo mode: synthetic storm loop, radar::MAX_FRAMES frames

    Scroller banner, condScroll, msgScroll, baroScroll;
    WeatherData wx;
    AlertView av;
    lightning::Status ls;
    uint32_t lastSlow = 0;
    uint8_t lastBri = 0;
    bool night = false;
    const themes::Theme* theme = nullptr;

    // ---- demo mode: synthetic data replaces the live data while the scenarios cycle ----
    struct Demo {
      bool on = false;
      uint8_t idx = 0;
      uint32_t nextAt = 0, endAt = 0, lastStrike = 0;
      WeatherData wx;
      AlertView av;
      lightning::Status ls;
      const themes::Theme* theme = nullptr;
      bool night = false, ringing = false, ringTimer = false, timer = false, lightningPage = false;
      bool sound = false, soundPending = false;
      uint8_t soundStyle = 0;
      const char* soundPhrase = nullptr;
      bool indoorSet = false;
      env_sensor::Reading indoor;
      uint32_t timerEnd = 0, lastRing = 0;
      uint8_t page = 255;          // 255 = rotate normally
      Screen screen = Screen::Composite;
      const char* name = "";
    } demo;
    constexpr uint32_t DEMO_STEP_MS = 8000;

    struct { String text; uint32_t until = 0; uint16_t color = 0xFFFF; bool active = false; } msg;   // written from other tasks
    SemaphoreHandle_t msgMtx = nullptr;
    bool lockMsg() { return msgMtx && xSemaphoreTake(msgMtx, pdMS_TO_TICKS(50)) == pdTRUE; }
    void unlockMsg() { xSemaphoreGive(msgMtx); }
    uint32_t strikeFlashUntil = 0, strikeX = 20;

    // ---- effects ---------------------------------------------------------------------------------------
    enum class Fx : uint8_t { None, Rain, Snow, Thunder, Confetti, Hearts, Sparkle };
    struct Particle { int16_t x, y16, vy16, vx16; uint16_t color; };
    constexpr uint8_t NP = 14;
    Particle parts[NP];
    Fx fxActive = Fx::None;
    uint32_t boltUntil = 0, nextBolt = 0;
    int16_t boltX = 30;

    const char* const DAY_NAMES[7] = { "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT" };
    const char* const MON_NAMES[12] = { "JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };
    constexpr uint16_t C_GREY = 0x7BEF, C_BOLT = 0xFFE0, C_ORANGE = 0xFC80;
    constexpr uint16_t C_SEV_EXTREME = 0xF800, C_SEV_SEVERE = 0xF800, C_SEV_MODERATE = 0xFC80, C_SEV_MINOR = 0xFFC0, C_SEV_UNKNOWN = 0x9CD3;

    uint16_t sevColor(Severity s) {
      switch (s) {
        case Severity::Extreme: return C_SEV_EXTREME;
        case Severity::Severe: return C_SEV_SEVERE;
        case Severity::Moderate: return C_SEV_MODERATE;
        case Severity::Minor: return C_SEV_MINOR;
        default: return C_SEV_UNKNOWN;
      }
    }
    uint16_t colTime() { return Canvas::rgb(theme ? theme->time : g_cfg.display.colors.time); }
    uint16_t colDate() { return Canvas::rgb(theme ? theme->date : g_cfg.display.colors.date); }
    uint16_t colText() { return Canvas::rgb(theme ? theme->text : g_cfg.display.colors.text); }

    void classicFont(Adafruit_GFX& c) { c.setFont(nullptr); c.setTextSize(1); c.setTextWrap(false); c.cp437(true); }
    void tinyFont(Adafruit_GFX& c) { c.setFont(&TomThumb); c.setTextSize(1); c.setTextWrap(false); }

    const char* compassName(int16_t deg) {
      static const char* const N[8] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
      return N[((deg + 22) / 45) & 7];
    }
    void fmtClock12(char* out, size_t n, int minuteOfDay) {
      int h = (minuteOfDay / 60) % 24, m = minuteOfDay % 60;
      if (g_cfg.time.use_24h) snprintf(out, n, "%02d:%02d", h, m);
      else { int h12 = h % 12; if (!h12) h12 = 12; snprintf(out, n, "%d:%02d%c", h12, m, h < 12 ? 'A' : 'P'); }
    }
    bool isRainCode(uint8_t w) { return (w >= 51 && w <= 67) || (w >= 80 && w <= 82); }
    bool isSnowCode(uint8_t w) { return (w >= 71 && w <= 77) || w == 85 || w == 86; }
    bool isThunderCode(uint8_t w) { return w >= 95 && w <= 99; }

    // ---- particles -----------------------------------------------------------------------------------
    void respawn(Particle& p, bool anywhere) {
      p.x = random(W);
      p.y16 = anywhere ? random(BOTTOM_H * 16) : 0;
      p.vx16 = 0;
      switch (fxActive) {
        case Fx::Rain: case Fx::Thunder: p.vy16 = 20 + random(20); p.color = Canvas::color565(40, 80, 200); break;
        case Fx::Snow: p.vy16 = 3 + random(5); p.vx16 = random(5) - 2; p.color = 0xC618; break;
        case Fx::Confetti: { static const uint16_t C[6] = { 0xF800, 0x07E0, 0x001F, 0xFFE0, 0xF81F, 0x07FF }; p.vy16 = 6 + random(10); p.vx16 = random(3) - 1; p.color = C[random(6)]; break; }
        case Fx::Hearts: p.vy16 = 4 + random(6); p.color = Canvas::color565(255, 70, 130); break;
        case Fx::Sparkle: p.y16 = random(BOTTOM_H * 16); p.vy16 = 10 + random(40); p.color = colDate(); break;   // vy16 = blink period
        default: break;
      }
    }
    void fxSelect(Fx f) {
      if (f == fxActive) return;
      fxActive = f;
      for (uint8_t i = 0; i < NP; i++) respawn(parts[i], true);
    }
    void drawBolt(Canvas& c, int16_t x, int16_t y, int16_t h, uint16_t color) {
      int16_t x1 = x - 2, y1 = y + h * 2 / 5, x2 = x + 1, y2 = y + h * 3 / 5, x3 = x - 2, y3 = y + h;
      c.drawLine(x, y, x1, y1, color);
      c.drawLine(x1, y1, x2, y2, color);
      c.drawLine(x2, y2, x3, y3, color);
    }
    void fxDraw(Canvas& c, uint32_t now) {
      if (fxActive == Fx::None) return;
      for (uint8_t i = 0; i < NP; i++) {
        Particle& p = parts[i];
        if (fxActive == Fx::Sparkle) {
          if (((now / 100) + i * 7) % (uint32_t)p.vy16 < 3) c.drawPixel(p.x, BOTTOM_Y + p.y16 / 16, p.color);
          if (random(400) == 0) respawn(p, true);
          continue;
        }
        p.y16 += p.vy16;
        p.x += (p.vx16 && (now / 200 + i) % 3 == 0) ? p.vx16 : 0;
        if (p.x < 0) p.x = W - 1;
        if (p.x >= W) p.x = 0;
        int16_t y = BOTTOM_Y + p.y16 / 16;
        if (y >= H) { respawn(p, false); continue; }
        if (fxActive == Fx::Rain || fxActive == Fx::Thunder) c.drawFastVLine(p.x, y, 2, p.color);
        else if (fxActive == Fx::Hearts) { c.drawPixel(p.x, y, p.color); c.drawPixel(p.x + 1, y, p.color); }
        else c.drawPixel(p.x, y, p.color);
      }
      if (fxActive == Fx::Thunder) {
        if ((int32_t)(now - nextBolt) >= 0) { nextBolt = now + 3000 + random(6000); boltUntil = now + 150; boltX = 4 + random(W - 8); }
        if ((int32_t)(now - boltUntil) < 0) drawBolt(c, boltX, BOTTOM_Y, BOTTOM_H - 1, ((now / 50) & 1) ? 0xFFFF : C_BOLT);
      }
    }

    // ---- clock ---------------------------------------------------------------------------------------
    void drawClock(Canvas& c, const struct tm& lt, bool valid, uint16_t ms) {
      const TimeConfig& tc = g_cfg.time;
      const uint16_t col = colTime();
      const bool annot = valid && (!tc.use_24h || tc.show_seconds);
      const int16_t x0 = annot ? TIME_X_LEFT : TIME_X_CENTERED;
      static const int16_t CELL_X[4] = { 0, 8, 21, 29 };
      if (!valid) {
        for (int i = 0; i < 4; i++) c.fillRect(x0 + CELL_X[i] + 1, TIME_Y + 5, 5, 2, C_GREY);
        digits7x11::draw(c, x0 + 16, TIME_Y, ':', C_GREY);
        return;
      }
      int hour = lt.tm_hour;
      bool pm = hour >= 12;
      if (!tc.use_24h) { hour %= 12; if (hour == 0) hour = 12; }
      char d[4] = { (char)('0' + hour / 10), (char)('0' + hour % 10), (char)('0' + lt.tm_min / 10), (char)('0' + lt.tm_min % 10) };
      if (!tc.use_24h && d[0] == '0') d[0] = ' ';
      for (int i = 0; i < 4; i++) digits7x11::draw(c, x0 + CELL_X[i], TIME_Y, d[i], col);
      if (!g_cfg.display.colon_blink || ms < 500) digits7x11::draw(c, x0 + 16, TIME_Y, ':', col);
      if (annot) {
        tinyFont(c);
        if (!tc.use_24h) c.drawText(pm ? "PM" : "AM", ANNOT_X, AMPM_Y, colText());
        if (tc.show_seconds) { char s[3]; snprintf(s, sizeof(s), "%02d", lt.tm_sec); c.drawText(s, ANNOT_X, SECS_Y, colText()); }
      }
      if (ls.active && g_cfg.lightning.show_bolt && ((ms < 700) || ((int32_t)(millis() - strikeFlashUntil) < 0)))
        drawBolt(c, W - 3, 1, 9, C_BOLT);
    }

    // ---- bottom-half content (drawn into a 64x16 page canvas) -------------------------------------------
    void drawIp(Canvas& pc) {
      classicFont(pc);
      String ip = wifi_mgr::ip().toString();
      int cut = ip.indexOf('.', ip.indexOf('.') + 1);
      pc.drawTextCentered((cut > 0 ? ip.substring(0, cut + 1) : ip).c_str(), W / 2, Y_L1, colDate());
      pc.drawTextCentered((cut > 0 ? ip.substring(cut + 1) : "").c_str(), W / 2, Y_L2, colText());
    }
    void drawStatusLine(Canvas& pc) {
      classicFont(pc);
      if (wifi_mgr::apActive() && !wifi_mgr::isConnected()) {
        pc.drawTextCentered("SETUP WIFI", W / 2, Y_L1, C_SEV_MINOR);
        pc.drawTextCentered("4.3.2.1", W / 2, Y_L2, colText());
      } else if (!wifi_mgr::isConnected()) pc.drawTextCentered("WIFI...", W / 2, Y_SINGLE, colText());
      else drawIp(pc);
    }
    void tempText(char* out, size_t n, float t, bool unit) {
      snprintf(out, n, "%d\xF8%s", (int)lroundf(t), unit ? (wx.imperial ? "F" : "C") : "");
    }
    void drawLightningPage(Canvas& pc) {
      classicFont(pc);
      pc.drawTextCentered("LIGHTNING", W / 2, Y_L1, C_BOLT);
      char l2[24];
      float d = ls.latest_km * (wx.imperial || !wx.valid ? 0.621371f : 1.0f);
      long ago = (long)(time(nullptr) - ls.latest_time);
      if (ago < 0) ago = 0;
      if (ago < 60) snprintf(l2, sizeof(l2), "%d%s %s NOW", (int)lroundf(d), wx.imperial || !wx.valid ? "MI" : "KM", lightning::bearingName(ls.latest_bearing));
      else snprintf(l2, sizeof(l2), "%d%s %s %ldM", (int)lroundf(d), wx.imperial || !wx.valid ? "MI" : "KM", lightning::bearingName(ls.latest_bearing), ago / 60);
      pc.drawTextCentered(l2, W / 2, Y_L2, colText());
    }
    constexpr uint16_t C_RISE = 0x3FE6, C_FALL = 0xFB00;      // trend arrows: soft green up, orange-red down
    // 3x5 arrow (double chevron when the change is fast); a grey dash when steady
    void drawTrend(Canvas& pc, int16_t x, int16_t y, env_sensor::Trend t) {
      if (t == env_sensor::Trend::Steady) { pc.drawFastHLine(x, y + 3, 3, C_GREY); return; }
      const bool up = (int8_t)t > 0, fast = (int8_t)t == 2 || (int8_t)t == -2;
      const uint16_t col = up ? C_RISE : C_FALL;
      auto head = [&](int16_t ty) { if (up) { pc.drawPixel(x + 1, ty, col); pc.drawFastHLine(x, ty + 1, 3, col); } else { pc.drawFastHLine(x, ty, 3, col); pc.drawPixel(x + 1, ty + 1, col); } };
      if (fast) { head(y + 1); head(y + 4); }
      else { head(y + 1); pc.drawFastVLine(x + 1, up ? y + 3 : y, 3, col); }
    }
    void drawHouseIcon(Canvas& pc, int16_t x, int16_t y) {
      const uint16_t roof = Canvas::rgb(0xE06030), wall = Canvas::rgb(0xF0D8A0), door = Canvas::rgb(0x6A3A1A), win = Canvas::rgb(0x80D0FF);
      for (int16_t i = 0; i < 7; i++) pc.drawFastHLine(x + 7 - i, y + 1 + i, 1 + 2 * i, roof);   // roof: rows 1..7
      pc.fillRect(x + 3, y + 8, 10, 7, wall);                                                    // body: rows 8..14
      pc.fillRect(x + 7, y + 10, 2, 5, door);
      pc.fillRect(x + 4, y + 9, 2, 2, win); pc.fillRect(x + 10, y + 9, 2, 2, win);
      pc.drawFastHLine(x + 2, y + 15, 12, C_GREY);                                              // ground
    }
    void drawIndoorPage(Canvas& pc, const env_sensor::Reading& r) {
      classicFont(pc);
      if (!r.valid) { pc.drawTextCentered(env_sensor::present() ? "READING.." : "NO SENSOR", W / 2, Y_SINGLE, C_GREY); return; }
      const bool imperial = g_cfg.weather.imperial;
      const uint16_t cTemp = Canvas::rgb(g_cfg.display.colors.temp), cText = colText();
      drawHouseIcon(pc, ICON_X, 0);
      char b[16];
      int16_t x = TEXT_X;
      snprintf(b, sizeof(b), "%d\xF8", (int)lroundf(imperial ? r.temp_c * 9.0f / 5.0f + 32.0f : r.temp_c));
      pc.drawText(b, x, Y_L1, cTemp);
      x += pc.textWidth(b);
      drawTrend(pc, x, Y_L1 + 1, r.t_temp);
      x += 5;
      if (r.has_humidity) {
        snprintf(b, sizeof(b), "%d%%", (int)lroundf(r.humidity));
        pc.drawText(b, x, Y_L1, cText);
        x += pc.textWidth(b);
        drawTrend(pc, x, Y_L1 + 1, r.t_hum);
      } else pc.drawText("IN", x, Y_L1, cText);
      // Second line: dew point, or the condensation / mould warning when there is one to give.
      if (r.has_humidity) {
        if (r.mould_risk) {
          pc.drawText(r.mould_risk >= 2 ? "MOULD RISK" : "DAMP AIR", TEXT_X, Y_L2, Canvas::rgb(r.mould_risk >= 2 ? 0xFF6060 : 0xFFC040));
        } else if (r.condensation) {
          pc.drawText(r.condensation >= 2 ? "CONDENSING" : "MAY CONDENSE", TEXT_X, Y_L2, Canvas::rgb(0x60C0FF));
        } else {
          snprintf(b, sizeof(b), "DEW %d\xF8", (int)lroundf(imperial ? r.dew_point_c * 9.0f / 5.0f + 32.0f : r.dew_point_c));
          pc.drawText(b, TEXT_X, Y_L2, cText);
        }
      }
    }
    constexpr uint16_t C_AIR_GOOD = 0x3FE6, C_AIR_FAIR = 0xFFC0, C_AIR_POOR = 0xF9C0;
    void drawAirPage(Canvas& pc, const env_sensor::Reading& r) {
      classicFont(pc);
      if (!r.valid || !r.has_gas) { pc.drawTextCentered(env_sensor::present() ? "NO GAS SENSOR" : "NO SENSOR", W / 2, Y_SINGLE, C_GREY); return; }
      if (!r.air_ready) { pc.drawTextCentered("AIR", W / 2, Y_L1, colDate()); pc.drawTextCentered("WARMING UP", W / 2, Y_L2, C_GREY); return; }
      const uint16_t col = r.air_level == env_sensor::AIR_GOOD ? C_AIR_GOOD : r.air_level == env_sensor::AIR_FAIR ? C_AIR_FAIR : C_AIR_POOR;
      char b[16];
      snprintf(b, sizeof(b), "AIR %d%%", (int)lroundf(r.air_score));
      int16_t tw = pc.textWidth(b);
      int16_t x = (int16_t)((W - tw - 5) / 2);
      pc.drawText(b, x, Y_L1, colText());
      drawTrend(pc, x + tw + 2, Y_L1 + 1, r.t_air);
      const char* word = r.air_level == env_sensor::AIR_GOOD ? "GOOD" : r.air_level == env_sensor::AIR_FAIR ? "FAIR" : "POOR";
      pc.drawTextCentered(word, W / 2, Y_L2, col);
    }
    // The sensor wins when there is one: it measures the air you are actually in, and it gives a real tendency.
    // Without a sensor the page falls back to the sea-level pressure the weather service reports, whose tendency
    // comes from a ring of past fetches kept in weather_client.
    bool baroSource(const env_sensor::Reading& r, float& hpa, float& d3h, uint16_t& span, bool& fromSensor) {
      if (r.valid && r.pressure_hpa > 0) {
        hpa = (g_cfg.indoor.sea_level && r.sea_level_known) ? r.sea_level_hpa : r.pressure_hpa;
        d3h = r.d_press;
        span = r.span_min;
        fromSensor = true;
        return true;
      }
      if (wx.valid && wx.cur.pressure > 0) {
        hpa = wx.cur.pressure;
        d3h = wx.cur.d_press_3h;
        span = wx.cur.press_span_min;
        fromSensor = false;
        return true;
      }
      return false;
    }
    env_sensor::Trend pressTrend(float d3h) {
      if (d3h >= 3.0f) return env_sensor::Trend::RisingFast;
      if (d3h >= 1.0f) return env_sensor::Trend::Rising;
      if (d3h <= -3.0f) return env_sensor::Trend::FallingFast;
      if (d3h <= -1.0f) return env_sensor::Trend::Falling;
      return env_sensor::Trend::Steady;
    }
    void drawBaroPage(Canvas& pc, const env_sensor::Reading& r, uint32_t now) {
      classicFont(pc);
      float hpa = 0, d3h = 0;
      uint16_t span = 0;
      bool fromSensor = false;
      if (!baroSource(r, hpa, d3h, span, fromSensor)) {
        pc.drawTextCentered(env_sensor::present() ? "READING.." : "NO PRESSURE", W / 2, Y_SINGLE, C_GREY);
        return;
      }
      const bool imperial = g_cfg.weather.imperial;
      const bool inhg = g_cfg.indoor.pressure_unit == 2 || (g_cfg.indoor.pressure_unit == 0 && imperial);
      char b[16];
      if (inhg) snprintf(b, sizeof(b), "%.2f IN", hpa * 0.02953f); else snprintf(b, sizeof(b), "%d HPA", (int)lroundf(hpa));
      int16_t tw = pc.textWidth(b);
      int16_t x = (int16_t)((W - tw - 5) / 2);
      pc.drawText(b, x, Y_L1, colText());
      drawTrend(pc, x + tw + 2, Y_L1 + 1, fromSensor ? r.t_press : pressTrend(d3h));
      // a small mark in the corner says where the number came from
      tinyFont(pc);
      pc.drawText(fromSensor ? "S" : "W", 1, Y_L1 + 2, C_GREY);
      classicFont(pc);
      const char* txt;
      if (span < 30) txt = "LEARNING TREND";
      else txt = zambretti::forecast(hpa, d3h, wx.valid ? wx.cur.wind_dir : -1).text;
      String up = txt; up.toUpperCase();
      baroScroll.setText(up.c_str(), 15, now);
      baroScroll.draw(pc, 0, Y_L2, W, colDate(), now, true);
    }
    // ---- moon ----
    // 13-pixel disc, lit side computed per row from the phase (waxing lit on the right, as seen from the north)
    void drawMoonIcon(Canvas& pc, int16_t x0, int16_t y0, float fraction) {
      const uint16_t lit = Canvas::rgb(0xEAEAD0), dark = Canvas::rgb(0x14141C);
      const float R = 6.5f, cx = 6.5f, cy = 6.5f;
      const float k = cosf(2.0f * (float)M_PI * fraction);
      for (int y = 0; y < 13; y++) {
        float dy = (y + 0.5f - cy) / R;
        if (dy <= -1.0f || dy >= 1.0f) continue;
        float half = sqrtf(1.0f - dy * dy) * R;
        float term = k * half;
        for (int x = 0; x < 13; x++) {
          float dx = x + 0.5f - cx;
          if (dx < -half || dx > half) continue;
          bool on = fraction < 0.5f ? (dx > term) : (dx < -term);
          pc.drawPixel(x0 + x, y0 + y, on ? lit : dark);
        }
      }
    }
    float demoMoonFraction = -1;   // < 0 = real moon
    void drawMoonPage(Canvas& pc, bool timeValid, uint32_t now) {
      classicFont(pc);
      moon::Info m;
      if (demo.on && demoMoonFraction >= 0) {
        m = moon::at(time(nullptr));
        m.fraction = demoMoonFraction; m.illumination = (1.0f - cosf(2.0f * (float)M_PI * demoMoonFraction)) / 2.0f;
        m.phase = demoMoonFraction < 0.5f ? moon::Phase::WaxingGibbous : moon::Phase::WaningCrescent; m.waxing = demoMoonFraction < 0.5f;
        m.days_to_full = 4.6f; m.days_to_new = 19.3f;
      } else if (!timeValid) { pc.drawTextCentered("MOON: NO TIME", W / 2, Y_SINGLE, C_GREY); return; }
      else m = moon::at(time(nullptr));
      drawMoonIcon(pc, 1, 1, m.fraction);
      const char* l1; const char* l2;
      switch (m.phase) {
        case moon::Phase::New: l1 = "NEW"; l2 = "MOON"; break;
        case moon::Phase::WaxingCrescent: l1 = "WAXING"; l2 = "CRESCENT"; break;
        case moon::Phase::FirstQuarter: l1 = "FIRST"; l2 = "QUARTER"; break;
        case moon::Phase::WaxingGibbous: l1 = "WAXING"; l2 = "GIBBOUS"; break;
        case moon::Phase::Full: l1 = "FULL"; l2 = "MOON"; break;
        case moon::Phase::WaningGibbous: l1 = "WANING"; l2 = "GIBBOUS"; break;
        case moon::Phase::LastQuarter: l1 = "LAST"; l2 = "QUARTER"; break;
        default: l1 = "WANING"; l2 = "CRESCENT"; break;
      }
      // the second line alternates between the phase word and the lit fraction / days to the next full or new moon
      char alt[12];
      const uint8_t slot = (uint8_t)((now / 4000) % 3);
      if (slot == 1) snprintf(alt, sizeof(alt), "%d%% LIT", (int)lroundf(m.illumination * 100));
      else if (slot == 2) { if (m.waxing && m.phase != moon::Phase::Full) snprintf(alt, sizeof(alt), "FULL %dD", (int)lroundf(m.days_to_full)); else snprintf(alt, sizeof(alt), "NEW %dD", (int)lroundf(m.days_to_new)); }
      pc.drawText(l1, 15, Y_L1, colDate());
      pc.drawText(slot ? alt : l2, 15, Y_L2, colText());
    }

    tide::Data demoTide;
    void buildDemoTide(time_t t) {
      demoTide = tide::Data(); demoTide.valid = true; demoTide.metric = !g_cfg.weather.imperial; demoTide.n = 4;
      strlcpy(demoTide.station, "8727012", sizeof(demoTide.station));
      const float k = demoTide.metric ? 0.3048f : 1.0f;
      demoTide.ex[0] = { t - 5 * 3600 - 20 * 60, 0.5f * k, false };
      demoTide.ex[1] = { t + 1 * 3600 + 40 * 60, 3.8f * k, true };
      demoTide.ex[2] = { t + 8 * 3600 + 5 * 60, 0.9f * k, false };
      demoTide.ex[3] = { t + 14 * 3600 + 30 * 60, 3.6f * k, true };
    }
    void drawTidePage(Canvas& pc, const tide::Data& d, bool timeValid) {
      classicFont(pc);
      if (!g_cfg.tide.station[0] && !demo.on) { pc.drawTextCentered("SET TIDE", W / 2, Y_L1, C_GREY); pc.drawTextCentered("STATION", W / 2, Y_L2, C_GREY); return; }
      tide::Extreme hi, lo; bool rising; float h;
      if (!timeValid || !tide::now(d, time(nullptr), hi, lo, rising, h)) { pc.drawTextCentered("NO TIDE DATA", W / 2, Y_SINGLE, C_GREY); return; }
      const tide::Extreme& first = hi.t < lo.t ? hi : lo;
      const tide::Extreme& second = hi.t < lo.t ? lo : hi;
      char t1[12], t2[12], l1[24], l2[24];
      struct tm a, b; localtime_r(&first.t, &a); localtime_r(&second.t, &b);
      fmtClock12(t1, sizeof(t1), a.tm_hour * 60 + a.tm_min);
      fmtClock12(t2, sizeof(t2), b.tm_hour * 60 + b.tm_min);
      snprintf(l1, sizeof(l1), "%s %s", first.high ? "HI" : "LO", t1);
      snprintf(l2, sizeof(l2), "%s %s", second.high ? "HI" : "LO", t2);
      const uint16_t cHi = Canvas::rgb(g_cfg.display.colors.hi), cLo = Canvas::rgb(g_cfg.display.colors.lo);
      pc.drawText(l1, 2, Y_L1, first.high ? cHi : cLo);
      pc.drawText(l2, 2, Y_L2, second.high ? cHi : cLo);
      // rising / falling arrow after the first line
      drawTrend(pc, (int16_t)(2 + pc.textWidth(l1) + 3), Y_L1 + 1, rising ? env_sensor::Trend::Rising : env_sensor::Trend::Falling);
      // height now in the small font after the second line, only when it fits (the Status tab carries the unit)
      const int16_t w2 = pc.textWidth(l2);
      tinyFont(pc);
      char hb[12];
      snprintf(hb, sizeof(hb), "%.1f", h);
      if (2 + w2 + 3 + pc.textWidth(hb) <= W) pc.drawTextRight(hb, W - 1, Y_L2 + 2, colText());
      classicFont(pc);
    }
    // ---- pages added in 0.14.0 ----
    void drawWaterPage(Canvas& pc, const tide::Data& d) {
      classicFont(pc);
      if (d.water_temp <= -999) { pc.drawTextCentered("NO WATER", W / 2, Y_L1, C_GREY); pc.drawTextCentered("TEMP", W / 2, Y_L2, C_GREY); return; }
      char l2[24];
      tempText(l2, sizeof(l2), d.water_temp, true);
      pc.drawTextCentered("WATER", W / 2, Y_L1, colDate());
      pc.drawTextCentered(l2, W / 2, Y_L2, Canvas::rgb(0x40C0FF));
    }
    void drawWorldPage(Canvas& pc) {
      classicFont(pc);
      struct tm z;
      if (!timesvc::zoneNow(g_cfg.time.tz2_posix, z)) { pc.drawTextCentered("NO 2ND ZONE", W / 2, Y_SINGLE, C_GREY); return; }
      char t[12];
      fmtClock12(t, sizeof(t), z.tm_hour * 60 + z.tm_min);
      const char* lbl = g_cfg.time.tz2_label[0] ? g_cfg.time.tz2_label : "ELSEWHERE";
      pc.drawTextCentered(lbl, W / 2, Y_L1, colDate());
      pc.drawTextCentered(t, W / 2, Y_L2, colText());
    }
    // Whole days from today to the event, in local time. Yearly events roll to next year once they are past.
    int32_t eventDaysAway(const EventConfig& e, const struct tm& today) {
      if (!e.enabled || !e.month || !e.day) return -1;
      struct tm a = {}, b = {};
      a.tm_year = today.tm_year; a.tm_mon = today.tm_mon; a.tm_mday = today.tm_mday; a.tm_hour = 12; a.tm_isdst = -1;
      b.tm_mon = e.month - 1; b.tm_mday = e.day; b.tm_hour = 12; b.tm_isdst = -1;
      b.tm_year = e.yearly ? today.tm_year : (int)e.year - 1900;
      timesvc::tzLock();
      time_t ta = mktime(&a), tb = mktime(&b);
      if (e.yearly && tb < ta) { b.tm_year = today.tm_year + 1; b.tm_isdst = -1; tb = mktime(&b); }
      timesvc::tzUnlock();
      if (ta <= 0 || tb <= 0) return -1;
      return (int32_t)lroundf((float)(tb - ta) / 86400.0f);
    }
    int8_t nextEvent(const struct tm& today, int32_t& daysOut) {
      int8_t best = -1;
      int32_t bestDays = 0;
      for (uint8_t i = 0; i < MAX_EVENTS; i++) {
        int32_t d = eventDaysAway(g_cfg.events.items[i], today);
        if (d < 0) continue;
        if (best < 0 || d < bestDays) { best = (int8_t)i; bestDays = d; }
      }
      daysOut = bestDays;
      return best;
    }
    void drawEventPage(Canvas& pc, const struct tm& lt, bool timeValid) {
      classicFont(pc);
      int32_t days = 0;
      const int8_t i = timeValid ? nextEvent(lt, days) : -1;
      if (i < 0) { pc.drawTextCentered("NO EVENTS", W / 2, Y_SINGLE, C_GREY); return; }
      const EventConfig& e = g_cfg.events.items[i];
      char l2[24];
      if (days == 0) snprintf(l2, sizeof(l2), "TODAY");
      else if (days == 1) snprintf(l2, sizeof(l2), "TOMORROW");
      else snprintf(l2, sizeof(l2), "%ld DAYS", (long)days);
      pc.drawTextCentered(e.label[0] ? e.label : "EVENT", W / 2, Y_L1, colDate());
      pc.drawTextCentered(l2, W / 2, Y_L2, days == 0 ? C_ORANGE : colText());
    }
    void drawStopwatch(Canvas& pc) {
      classicFont(pc);
      const uint32_t ms = alarmclock::stopwatchMs();
      const uint32_t s = ms / 1000;
      char t[16];
      if (s >= 3600) snprintf(t, sizeof(t), "%lu:%02lu:%02lu", (unsigned long)(s / 3600), (unsigned long)(s % 3600 / 60), (unsigned long)(s % 60));
      else snprintf(t, sizeof(t), "%lu:%02lu.%lu", (unsigned long)(s / 60), (unsigned long)(s % 60), (unsigned long)(ms % 1000 / 100));
      pc.drawTextCentered(alarmclock::stopwatchRunning() ? "STOPWATCH" : "STOPPED", W / 2, Y_L1, colDate());
      pc.drawTextCentered(t, W / 2, Y_L2, colText());
    }
    // a page in the rotation is shown only when it has something to show
    bool pageAvailable(uint8_t id) {
      if (demo.on) return true;
      switch (id) {
        case PAGE_INDOOR: return env_sensor::present();
        case PAGE_BARO: return env_sensor::present() || (wx.valid && wx.cur.pressure > 0);
        case PAGE_AIR: return env_sensor::hasGas();
        case PAGE_TIDE: { tide::Data td; return g_cfg.tide.enabled && tide::get(td); }
        case PAGE_WATER: { tide::Data td; return g_cfg.tide.enabled && g_cfg.tide.water_temp && tide::get(td) && td.water_temp > -999; }
        case PAGE_UV: return wx.valid && wx.cur.uv >= 0;
        case PAGE_SKY: return wx.valid && (wx.cur.cloud >= 0 || wx.cur.vis >= 0);
        case PAGE_RAIN: return wx.valid && wx.ndaily;
        case PAGE_WORLD: return g_cfg.time.tz2_posix[0] != '\0';
        case PAGE_EVENT: { struct tm lt; int32_t d; return timesvc::localNow(lt) && nextEvent(lt, d) >= 0; }
        default: return true;
      }
    }
    void drawPage(Canvas& pc, uint8_t id, const struct tm& lt, bool timeValid, uint32_t now) {
      classicFont(pc);
      if (id == PAGE_MOON) { drawMoonPage(pc, timeValid, now); return; }
      if (id == PAGE_TIDE) { tide::Data td; if (demo.on) td = demoTide; else tide::get(td); drawTidePage(pc, td, timeValid || demo.on); return; }
      if (id == PAGE_INDOOR) { drawIndoorPage(pc, demo.on && demo.indoorSet ? demo.indoor : env_sensor::reading()); return; }
      if (id == PAGE_AIR) { drawAirPage(pc, demo.on && demo.indoorSet ? demo.indoor : env_sensor::reading()); return; }
      if (id == PAGE_BARO) { drawBaroPage(pc, demo.on && demo.indoorSet ? demo.indoor : env_sensor::reading(), now); return; }
      if (id == PAGE_WATER) { tide::Data td; if (demo.on) td = demoTide; else tide::get(td); drawWaterPage(pc, td); return; }
      if (id == PAGE_WORLD) { drawWorldPage(pc); return; }
      if (id == PAGE_EVENT) { drawEventPage(pc, lt, timeValid || demo.on); return; }
      const uint16_t cText = colText(), cTemp = Canvas::rgb(g_cfg.display.colors.temp), cDate = colDate();
      char l1[32], l2[32];
      if (id == PAGE_DATE) {
        if (!timeValid) { drawStatusLine(pc); return; }
        snprintf(l1, sizeof(l1), "%s", DAY_NAMES[lt.tm_wday % 7]);
        if (g_cfg.time.date_order == 1) snprintf(l2, sizeof(l2), "%d %s", lt.tm_mday, MON_NAMES[lt.tm_mon % 12]);
        else if (g_cfg.time.date_order == 2) snprintf(l2, sizeof(l2), "%02d-%02d", lt.tm_mon + 1, lt.tm_mday);
        else snprintf(l2, sizeof(l2), "%s %d", MON_NAMES[lt.tm_mon % 12], lt.tm_mday);
        pc.drawTextCentered(l1, W / 2, Y_L1, cDate);
        pc.drawTextCentered(l2, W / 2, Y_L2, cText);
        return;
      }
      if (!wx.valid) { pc.drawTextCentered(wifi_mgr::isConnected() ? "WEATHER.." : "NO WIFI", W / 2, Y_SINGLE, C_GREY); return; }
      wmo::Icon ic = wmo::icon(wx.cur.wmo, wx.cur.is_day);
      switch (id) {
        case PAGE_TEMP:
          wmo::drawIcon(pc, ICON_X, 0, ic);
          tempText(l1, sizeof(l1), wx.cur.temp, true);
          snprintf(l2, sizeof(l2), "H %d%%", (int)lroundf(wx.cur.humidity));
          pc.drawText(l1, TEXT_X, Y_L1, cTemp);
          pc.drawText(l2, TEXT_X, Y_L2, cText);
          break;
        case PAGE_COND:
          condScroll.setText(wmo::text(wx.cur.wmo), 15, now);
          condScroll.draw(pc, TEXT_X, Y_SINGLE, W - TEXT_X, cText, now, true);
          wmo::drawIcon(pc, ICON_X, 0, ic);
          break;
        case PAGE_WIND:
          snprintf(l1, sizeof(l1), "WIND %s", compassName(wx.cur.wind_dir));
          snprintf(l2, sizeof(l2), "%d GUST %d", (int)lroundf(wx.cur.wind), (int)lroundf(wx.cur.gust));
          if (pc.textWidth(l2) > W - 2) snprintf(l2, sizeof(l2), "%d G %d", (int)lroundf(wx.cur.wind), (int)lroundf(wx.cur.gust));
          pc.drawTextCentered(l1, W / 2, Y_L1, cDate);
          pc.drawTextCentered(l2, W / 2, Y_L2, cText);
          break;
        case PAGE_HILO:
          if (wx.ndaily) {
            wmo::drawIcon(pc, ICON_X, 0, wmo::icon(wx.daily[0].wmo, true));
            tempText(l1, sizeof(l1), wx.daily[0].tmax, false);
            tempText(l2, sizeof(l2), wx.daily[0].tmin, false);
            char h[40], l[40];
            snprintf(h, sizeof(h), "H %s", l1);
            snprintf(l, sizeof(l), "L %s", l2);
            pc.drawText(h, TEXT_X, Y_L1, Canvas::rgb(g_cfg.display.colors.hi));
            pc.drawText(l, TEXT_X, Y_L2, Canvas::rgb(g_cfg.display.colors.lo));
          }
          break;
        case PAGE_FEELS:
          wmo::drawIcon(pc, ICON_X, 0, ic);
          tempText(l2, sizeof(l2), wx.cur.feels, true);
          pc.drawText("FEELS", TEXT_X, Y_L1, cDate);
          pc.drawText(l2, TEXT_X, Y_L2, cTemp);
          break;
        case PAGE_SUN: {
          if (wx.sunrise_min < 0) { pc.drawTextCentered("NO SUN DATA", W / 2, Y_SINGLE, C_GREY); break; }
          char t[12];
          fmtClock12(t, sizeof(t), wx.sunrise_min);
          snprintf(l1, sizeof(l1), "RISE %s", t);
          fmtClock12(t, sizeof(t), wx.sunset_min);
          snprintf(l2, sizeof(l2), "SET %s", t);
          pc.drawTextCentered(l1, W / 2, Y_L1, Canvas::rgb(0xFFD060));
          pc.drawTextCentered(l2, W / 2, Y_L2, Canvas::rgb(0xFF8060));
          break;
        }
        case PAGE_UV: {
          if (wx.cur.uv < 0) { pc.drawTextCentered("NO UV DATA", W / 2, Y_SINGLE, C_GREY); break; }
          // WHO bands: 0-2 low, 3-5 moderate, 6-7 high, 8-10 very high, 11+ extreme
          const float u = wx.cur.uv;
          const uint16_t cu = Canvas::rgb(u < 3 ? 0x40C060 : u < 6 ? 0xFFD060 : u < 8 ? 0xFF8020 : u < 11 ? 0xFF4040 : 0xC040FF);
          snprintf(l1, sizeof(l1), "UV %.0f", u);
          const char* band = u < 3 ? "LOW" : u < 6 ? "MODERATE" : u < 8 ? "HIGH" : u < 11 ? "V HIGH" : "EXTREME";
          if (wx.ndaily && wx.daily[0].uv_max >= 0 && u < wx.daily[0].uv_max - 0.5f)
            snprintf(l2, sizeof(l2), "%s MAX %.0f", band, wx.daily[0].uv_max);
          else snprintf(l2, sizeof(l2), "%s", band);
          if (pc.textWidth(l2) > W - 2) snprintf(l2, sizeof(l2), "%s", band);
          pc.drawTextCentered(l1, W / 2, Y_L1, cu);
          pc.drawTextCentered(l2, W / 2, Y_L2, cText);
          break;
        }
        case PAGE_SKY: {
          if (wx.cur.cloud < 0 && wx.cur.vis < 0) { pc.drawTextCentered("NO SKY DATA", W / 2, Y_SINGLE, C_GREY); break; }
          if (wx.cur.cloud >= 0) snprintf(l1, sizeof(l1), "CLOUD %d%%", (int)lroundf(wx.cur.cloud));
          else l1[0] = '\0';
          if (wx.cur.vis >= 0) snprintf(l2, sizeof(l2), "VIS %d %s", (int)lroundf(wx.cur.vis), wx.imperial ? "MI" : "KM");
          else l2[0] = '\0';
          if (l1[0]) pc.drawTextCentered(l1, W / 2, Y_L1, cDate);
          if (l2[0]) pc.drawTextCentered(l2, W / 2, l1[0] ? Y_L2 : Y_SINGLE, cText);
          break;
        }
        case PAGE_RAIN: {
          if (!wx.ndaily) { pc.drawTextCentered("NO RAIN DATA", W / 2, Y_SINGLE, C_GREY); break; }
          const float today = wx.daily[0].rain_sum;
          if (today < 0) snprintf(l1, sizeof(l1), "RAIN --");
          else if (wx.imperial) snprintf(l1, sizeof(l1), "RAIN %.2f\"", today);
          else snprintf(l1, sizeof(l1), "RAIN %.1fMM", today);
          snprintf(l2, sizeof(l2), "CHANCE %d%%", (int)wx.daily[0].pop);
          pc.drawTextCentered(l1, W / 2, Y_L1, Canvas::rgb(0x60A0FF));
          pc.drawTextCentered(l2, W / 2, Y_L2, cText);
          break;
        }
        default: break;
      }
    }
    void drawBanner(Canvas& pc, uint32_t now) {
      String txt;
      for (uint8_t i = 0; i < av.n; i++) {
        if (i) txt += "  *  ";
        txt += av.items[i].event; txt += ": "; txt += av.items[i].headline;
      }
      classicFont(pc);
      banner.setText(txt.c_str(), g_cfg.alerts.banner_px_per_s, now);
      banner.draw(pc, 0, Y_BANNER, W, sevColor(av.top), now);
      if (av.stale) pc.drawPixel(W - 1, BOTTOM_H - 1, C_GREY);
    }
    void drawMessage(Canvas& pc, uint32_t now) {
      String text; uint16_t color = 0xFFFF;
      if (lockMsg()) { text = msg.text; color = msg.color; unlockMsg(); }
      classicFont(pc);
      msgScroll.setText(text.c_str(), 22, now);
      msgScroll.draw(pc, 0, Y_BANNER, W, color, now, true);
    }
    void drawTimer(Canvas& pc, uint32_t s) {
      classicFont(pc);
      char t[16];
      if (s >= 3600) snprintf(t, sizeof(t), "%lu:%02lu:%02lu", (unsigned long)(s / 3600), (unsigned long)(s % 3600 / 60), (unsigned long)(s % 60));
      else snprintf(t, sizeof(t), "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
      pc.drawTextCentered("TIMER", W / 2, Y_L1, colDate());
      pc.drawTextCentered(t, W / 2, Y_L2, colText());
    }
    void drawRing(Canvas& pc, uint32_t now, bool timer, const char* label) {
      classicFont(pc);
      const bool on = (now / 500) & 1;
      pc.drawTextCentered(timer ? "TIMER" : "ALARM", W / 2, Y_L1, on ? C_ORANGE : 0xFFFF);
      pc.drawTextCentered(timer ? "DONE" : (label && *label ? label : "WAKE UP"), W / 2, Y_L2, colText());
    }

    // ---- demo scenarios ----
    WeatherData demoWeather(uint8_t wmo, bool day, float temp, float feels, float hum, float wind, float gust, int16_t dir) {
      WeatherData w;
      w.valid = true; w.imperial = g_cfg.weather.imperial; w.fetched_ms = millis();
      auto tc = [&](float f) { return w.imperial ? f : (f - 32.0f) * 5.0f / 9.0f; };
      w.cur.wmo = wmo; w.cur.is_day = day; w.cur.temp = tc(temp); w.cur.feels = tc(feels); w.cur.humidity = hum;
      w.cur.wind = w.imperial ? wind : wind * 1.609f; w.cur.gust = w.imperial ? gust : gust * 1.609f; w.cur.wind_dir = dir;
      const char* dates[3] = { "2026-09-24", "2026-09-25", "2026-09-26" };
      const uint8_t codes[3] = { 0, 2, 61 }; const float hi[3] = { 75, 71, 64 }, lo[3] = { 55, 52, 49 }; const uint8_t pop[3] = { 5, 20, 70 };
      for (int i = 0; i < 3; i++) { strlcpy(w.daily[i].date, dates[i], sizeof(w.daily[i].date)); w.daily[i].wmo = codes[i]; w.daily[i].tmax = tc(hi[i]); w.daily[i].tmin = tc(lo[i]); w.daily[i].pop = pop[i]; }
      w.ndaily = 3;
      const float ht[12] = { 72, 74, 75, 73, 70, 66, 63, 61, 59, 58, 57, 56 }; const uint8_t hp[12] = { 0, 0, 5, 10, 20, 35, 55, 60, 40, 20, 10, 5 };
      for (int i = 0; i < 12; i++) { w.hourly[i].hour = (int8_t)((14 + i) % 24); w.hourly[i].temp = tc(ht[i]); w.hourly[i].pop = hp[i]; }
      w.nhourly = 12;
      w.sunrise_min = 6 * 60 + 48; w.sunset_min = 18 * 60 + 55;
      return w;
    }
    void demoAlert(const char* event, const char* headline, Severity sev, bool fresh, uint32_t now) {
      demo.av = AlertView();
      demo.av.n = 1;
      strlcpy(demo.av.items[0].event, event, sizeof(demo.av.items[0].event));
      strlcpy(demo.av.items[0].headline, headline, sizeof(demo.av.items[0].headline));
      demo.av.items[0].sev = sev; demo.av.items[0].first_seen_ms = fresh ? now : now - 120000UL;
      demo.av.top = sev; demo.av.newest_ms = fresh ? now : 0;
    }
    constexpr uint8_t DEMO_COUNT = 28;
    void buildDemoRadar();
    void demoApply(uint8_t i, uint32_t now) {
      demo.idx = i;
      demo.av = AlertView(); demo.ls = lightning::Status(); demo.theme = nullptr;
      demo.night = demo.ringing = demo.ringTimer = demo.timer = demo.lightningPage = demo.indoorSet = false;
      demoMoonFraction = -1;
      demo.page = 255; demo.screen = Screen::Composite;
      demo.wx = demoWeather(0, true, 72, 70, 46, 12, 19, 315);
      clearMessage();
      switch (i) {
        case 0:  demo.name = "sunny";      demo.page = PAGE_TEMP; break;
        case 1:  demo.name = "date";       demo.page = PAGE_DATE; break;
        case 2:  demo.name = "rain";       demo.wx = demoWeather(63, true, 58, 55, 91, 9, 14, 200); demo.page = PAGE_COND; break;
        case 3:  demo.name = "snow";       demo.wx = demoWeather(73, true, 28, 19, 88, 11, 18, 350); demo.page = PAGE_TEMP; break;
        case 4:  demo.name = "thunder";    demo.wx = demoWeather(95, true, 68, 70, 80, 22, 41, 240); demo.page = PAGE_COND; break;
        case 5:  demo.name = "lightning";  demo.wx = demoWeather(95, true, 70, 72, 82, 18, 33, 250);
                 demo.ls.enabled = demo.ls.connected = demo.ls.active = true; demo.ls.count = 4; demo.ls.nearest_km = 9.4f; demo.ls.latest_km = 12.9f;
                 demo.ls.latest_bearing = 300; demo.ls.latest_time = time(nullptr) - 120; demo.lightningPage = true; break;
        case 6:  demo.name = "wind";       demo.wx = demoWeather(2, true, 66, 61, 40, 27, 38, 290); demo.page = PAGE_WIND; break;
        case 7:  demo.name = "high-low";   demo.page = PAGE_HILO; break;
        case 8:  demo.name = "sun";        demo.page = PAGE_SUN; break;
        case 9:  demo.name = "forecast";   demo.screen = Screen::Forecast; break;
        case 10: demo.name = "hourly";     demo.screen = Screen::Hourly; break;
        case 11: demo.name = "tornado warning"; demo.wx = demoWeather(95, true, 74, 76, 78, 30, 55, 210);
                 demoAlert("Tornado Warning", "Tornado Warning until 5:00 PM by NWS - take shelter now", Severity::Extreme, true, now); break;
        case 12: demo.name = "winter storm watch"; demo.wx = demoWeather(71, true, 30, 22, 84, 14, 22, 20);
                 demoAlert("Winter Storm Watch", "Winter Storm Watch from Friday evening through Saturday afternoon", Severity::Moderate, false, now); break;
        case 13: demo.name = "alarm";      demo.ringing = true; break;
        case 14: demo.name = "timer";      demo.timer = true; demo.timerEnd = now + 754000UL; break;
        case 15: demo.name = "timer done"; demo.ringing = true; demo.ringTimer = true; break;
        case 16: demo.name = "message";    showMessage("Demo mode - messages scroll here", DEMO_STEP_MS, 0x40C0FF); break;
        case 17: demo.name = "christmas";  demo.wx = demoWeather(3, true, 34, 28, 70, 8, 12, 10); demo.theme = themes::forDate(themes::sample(themes::Sample::Christmas)); demo.page = PAGE_DATE; break;
        case 18: demo.name = "july 4th";   demo.wx = demoWeather(0, true, 88, 90, 35, 6, 10, 180); demo.theme = themes::forDate(themes::sample(themes::Sample::July4)); demo.page = PAGE_DATE; break;
        case 19: demo.name = "valentine";  demo.theme = themes::forDate(themes::sample(themes::Sample::Valentine)); demo.page = PAGE_TEMP; break;
        case 20: demo.name = "halloween";  demo.wx = demoWeather(2, false, 52, 49, 60, 5, 9, 90); demo.theme = themes::forDate(themes::sample(themes::Sample::Halloween)); demo.page = PAGE_DATE; break;
        case 21: demo.name = "night mode"; demo.night = true; break;
        case 22: demo.name = "indoor";     demo.page = PAGE_INDOOR; demo.indoorSet = true; demo.indoor = env_sensor::Reading();
                 demo.indoor.valid = demo.indoor.has_humidity = demo.indoor.sea_level_known = true;
                 demo.indoor.temp_c = 22.1f; demo.indoor.humidity = 44; demo.indoor.pressure_hpa = 1009.2f; demo.indoor.sea_level_hpa = 1013.6f;
                 demo.indoor.t_temp = env_sensor::Trend::Rising; demo.indoor.t_hum = env_sensor::Trend::Falling; demo.indoor.t_press = env_sensor::Trend::FallingFast;
                 demo.indoor.d_temp = 0.8f; demo.indoor.d_hum = -4; demo.indoor.d_press = -3.4f; demo.indoor.span_min = 180; break;
        case 23: demo.name = "radar";      demo.wx = demoWeather(63, true, 58, 55, 91, 9, 14, 200); demo.screen = Screen::Radar; buildDemoRadar(); break;
        case 24: demo.name = "air quality"; demo.page = PAGE_AIR; demo.indoorSet = true; demo.indoor = env_sensor::Reading();
                 demo.indoor.valid = demo.indoor.has_humidity = demo.indoor.has_gas = demo.indoor.gas_valid = demo.indoor.air_ready = true;
                 demo.indoor.temp_c = 23.4f; demo.indoor.humidity = 52; demo.indoor.pressure_hpa = 1013.0f; demo.indoor.sea_level_hpa = 1013.0f;
                 demo.indoor.gas_kohm = 61.2f; demo.indoor.air_baseline_kohm = 148.0f; demo.indoor.air_score = 66; demo.indoor.air_level = env_sensor::AIR_FAIR;
                 demo.indoor.t_air = env_sensor::Trend::FallingFast; demo.indoor.d_air = -27; demo.indoor.span_min = 60; break;
        case 25: demo.name = "barometer";  demo.wx = demoWeather(3, true, 64, 62, 78, 16, 26, 190); demo.page = PAGE_BARO; demo.indoorSet = true; demo.indoor = env_sensor::Reading();
                 demo.indoor.valid = demo.indoor.has_humidity = demo.indoor.sea_level_known = true;
                 demo.indoor.temp_c = 21.0f; demo.indoor.humidity = 58; demo.indoor.pressure_hpa = 1004.6f; demo.indoor.sea_level_hpa = 1008.9f;
                 demo.indoor.t_press = env_sensor::Trend::FallingFast; demo.indoor.d_press = -3.8f; demo.indoor.span_min = 180; break;
        case 26: demo.name = "tide";       demo.wx = demoWeather(1, true, 79, 78, 49, 9, 15, 250); demo.page = PAGE_TIDE; buildDemoTide(time(nullptr)); break;
        case 27: demo.name = "moon";       demo.wx = demoWeather(0, false, 61, 59, 55, 4, 7, 20); demo.page = PAGE_MOON; demoMoonFraction = 0.38f; break;
        default: demo.name = "sunny"; demo.page = PAGE_TEMP; break;
      }
      // sounds that a real event would produce (alert chime, lightning chime, message chime, alarm beeps) plus the
      // spoken scenario name from the voice pack; alarm scenarios keep repeating their beeps in tick()
      static const char* const SPOKEN[DEMO_COUNT] = {
        "sunny", "date", "rain", "snow", "thunderstorm", "lightning nearby", "wind", "high and low", "sunrise and sunset",
        "forecast", "hourly graph", "tornado warning", "winter storm watch", "alarm", "timer", "timer finished", "message",
        "christmas", "fourth of july", "valentine's day", "halloween", "night mode", "indoor", "radar", "air quality", "barometer", "tide", "moon" };
      demo.lastRing = 0;
      if (demo.sound) {
        demo.soundPending = true;
        demo.soundPhrase = i < DEMO_COUNT ? SPOKEN[i] : nullptr;
        ChimeStyle cs = ChimeStyle::None;
        if (i == 11) cs = g_cfg.audio.chime_extreme;
        else if (i == 5 || i == 16) cs = g_cfg.audio.chime;
        else if (i == 13 || i == 15) { cs = ChimeStyle::TripleBeep; demo.lastRing = now; }
        demo.soundStyle = (uint8_t)cs;
      }
      if (demo.screen != Screen::Composite) { screen = demo.screen; screenUntil = now + DEMO_STEP_MS; transFrom = 255; radarIdx = 0; radarLoaded = false; radarNextAt = now; radarBaseOk = radarBase && g_cfg.radar.base_map && radar::copyBase(radarBase); }
      else if (screen == Screen::Forecast || screen == Screen::Hourly) screen = Screen::Composite;
      pageSince = now;
      transFrom = 255;
    }

    // copies the page canvas into the bottom half; black is transparent so effects show behind the content
    void blitBottom(Canvas& c, const Canvas& pc, int16_t dx) {
      const uint16_t* src = pc.getBuffer();
      for (int16_t y = 0; y < BOTTOM_H; y++) {
        for (int16_t x = 0; x < W; x++) {
          uint16_t v = src[y * W + x];
          if (!v) continue;
          int16_t tx = x + dx;
          if (tx < 0 || tx >= W) continue;
          c.drawPixel(tx, BOTTOM_Y + y, v);
        }
      }
    }

    // ---- radar ----------------------------------------------------------------------------------------
    // NWS reflectivity palette (5 dBZ steps from 5 to 75) used for the demo storm
    const uint16_t DBZ_COL[15] = { 0x0774, 0x04FE, 0x001E, 0x07E0, 0x0620, 0x0460, 0xFFC0, 0xE5E0, 0xFCA0, 0xF800, 0xD000, 0xB800, 0xF81F, 0x9A98, 0xFFFF };
    void buildDemoRadar() {
      if (!demoRadar) demoRadar = (uint16_t*)heap_caps_malloc(radar::MAX_FRAMES * radar::W * radar::H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (!demoRadar) return;
      for (uint8_t k = 0; k < radar::MAX_FRAMES; k++) {
        uint16_t* f = demoRadar + (size_t)k * radar::W * radar::H;
        memset(f, 0, radar::W * radar::H * 2);
        const float cx = 6.0f + k * 4.6f, cy = 27.0f - k * 1.4f;         // storm cell moving to the north-east
        const float cx2 = 20.0f + k * 3.8f, cy2 = 6.0f + k * 0.6f;       // a smaller cell ahead of it
        for (int y = 0; y < radar::H; y++) for (int x = 0; x < radar::W; x++) {
          float dx = (x - cx) / 13.0f, dy = (y - cy) / 7.5f;
          float d = sqrtf(dx * dx + dy * dy);
          float dx2 = (x - cx2) / 6.0f, dy2 = (y - cy2) / 4.0f;
          float d2 = sqrtf(dx2 * dx2 + dy2 * dy2);
          float lvl = 0;
          if (d < 1.0f) lvl = (1.0f - d) * 13.0f + ((x * 7 + y * 3 + k) % 5) * 0.4f;       // up to ~65 dBZ in the core
          if (d2 < 1.0f) lvl = max(lvl, (1.0f - d2) * 7.0f);
          if (lvl < 0.8f && ((x * 31 + y * 17 + k * 5) % 23) == 0 && d < 1.6f) lvl = 1;    // scattered light returns
          if (lvl >= 0.8f) f[y * radar::W + x] = DBZ_COL[min(14, (int)lvl)];
        }
      }
    }
    bool isPrecipCode(uint8_t c) { return (c >= 51 && c <= 67) || (c >= 71 && c <= 77) || (c >= 80 && c <= 86) || c >= 95; }
    bool radarAvailable() { return demo.on ? demoRadar != nullptr : (g_cfg.radar.enabled && radar::frameCount() > 0); }
    void startRadar(uint32_t now) {
      screen = Screen::Radar;
      radarIdx = 0; radarLoaded = false; radarNextAt = now;
      radarBaseOk = radarBase && g_cfg.radar.base_map && radar::copyBase(radarBase);
      screenUntil = now + (uint32_t)g_cfg.radar.show_sec * 1000UL;
      transFrom = 255;
    }
    void drawRadar(Canvas& c, uint32_t now) {
      const uint8_t n = demo.on ? radar::MAX_FRAMES : radar::frameCount();
      if (!n || !radarFrame || !radarBase) { classicFont(c); c.drawTextCentered("NO RADAR", W / 2, 12, C_GREY); return; }
      if (radarIdx >= n) radarIdx = 0;
      if ((int32_t)(now - radarNextAt) >= 0) {
        if (radarLoaded) { radarIdx = (uint8_t)((radarIdx + 1) % n); }
        radarLoaded = false;
        radarNextAt = now + (radarIdx == n - 1 ? (uint32_t)g_cfg.radar.frame_ms + g_cfg.radar.hold_ms : (uint32_t)g_cfg.radar.frame_ms);
      }
      if (!radarLoaded) {
        if (demo.on) memcpy(radarFrame, demoRadar + (size_t)radarIdx * RADAR_PX, RADAR_PX * 2);
        else if (!radar::copyFrame(radarIdx, radarFrame)) memset(radarFrame, 0, RADAR_PX * 2);
        radarLoaded = true;
      }
      // base map underneath (dim water tint, land stays dark, grey coastline), echoes on top; the values are picked
      // for the panel's 2.2 gamma so the tint reads as a quiet blue and the coast as a thin grey line
      constexpr uint16_t C_WATER = 0x0010, C_LAND = 0x0000, C_COAST = 0x8410;
      const uint8_t mode = g_cfg.radar.base_map & 3;
      for (int y = 0; y < radar::H; y++) for (int x = 0; x < radar::W; x++) {
        const int i = y * radar::W + x;
        uint16_t v = radarFrame[i];
        if (!v && radarBaseOk) {
          const uint8_t m = radarBase[i];
          if ((mode & 1) && (m & radar::BASE_COAST)) v = C_COAST;
          else if (mode & 2) v = (m & radar::BASE_WATER) ? C_WATER : ((m & radar::BASE_LAND) ? C_LAND : 0);
        }
        if (v) c.drawPixel(x, y, v);
      }
      // home marker: a small blinking cross
      const bool on = (now / 350) & 1;
      const uint16_t mk = on ? 0xFFFF : 0x0000;
      c.drawPixel(W / 2, H / 2, mk);
      c.drawPixel(W / 2 - 1, H / 2, mk); c.drawPixel(W / 2 + 1, H / 2, mk); c.drawPixel(W / 2, H / 2 - 1, mk); c.drawPixel(W / 2, H / 2 + 1, mk);
      // age of the frame, bottom left
      tinyFont(c);
      char lab[8];
      int32_t age = demo.on ? (int32_t)(radar::MAX_FRAMES - 1 - radarIdx) * 5 : radar::frameAgeMin(radarIdx);
      if (radarIdx == n - 1 || age <= 2) strlcpy(lab, "NOW", sizeof(lab)); else snprintf(lab, sizeof(lab), "-%ldM", (long)age);
      int16_t tw = c.textWidth(lab);
      c.fillRect(0, H - 6, tw + 2, 6, 0x0000);
      c.drawText(lab, 1, H - 6, radarIdx == n - 1 ? 0xFFFF : C_GREY);
    }

    // ---- full screens ----------------------------------------------------------------------------------
    void drawForecast(Canvas& c) {
      for (uint8_t i = 0; i < wx.ndaily && i < 3; i++) {
        const WeatherDaily& d = wx.daily[i];
        int16_t x = i * FC_COL_W + 1;
        int y = 0, m = 0, dd = 0;
        const char* name = "---";
        if (sscanf(d.date, "%4d-%2d-%2d", &y, &m, &dd) == 3) {
          int64_t days = days_from_civil(y, (unsigned)m, (unsigned)dd);
          name = DAY_NAMES[(int)(((days % 7) + 7 + 4) % 7)];
        }
        tinyFont(c);
        c.drawTextCentered(name, x + FC_COL_W / 2 - 1, FC_DAY_Y, colDate());
        wmo::drawIcon(c, x + 2, FC_ICON_Y, wmo::icon(d.wmo, true));
        char hi[8], lo[8];
        snprintf(hi, sizeof(hi), "%d", (int)lroundf(d.tmax));
        snprintf(lo, sizeof(lo), "%d", (int)lroundf(d.tmin));
        tinyFont(c);
        c.drawText(hi, x, FC_TEMP_Y, Canvas::rgb(g_cfg.display.colors.hi));
        c.drawTextRight(lo, x + FC_COL_W - 2, FC_TEMP_Y, Canvas::rgb(g_cfg.display.colors.lo));
        if (i) c.drawFastVLine(x - 1, 1, H - 2, 0x2104);
      }
    }
    // next 12 hours: temperature curve on top, rain chance bars below
    void drawHourly(Canvas& c) {
      const uint8_t n = wx.nhourly;
      if (n < 2) return;
      float tmin = 1e9f, tmax = -1e9f;
      for (uint8_t i = 0; i < n; i++) { tmin = min(tmin, wx.hourly[i].temp); tmax = max(tmax, wx.hourly[i].temp); }
      float range = max(tmax - tmin, 4.0f);
      const int16_t x0 = 13, x1 = W - 2, top = 2, bot = 15;
      auto px = [&](uint8_t i) { return (int16_t)(x0 + (int32_t)i * (x1 - x0) / (n - 1)); };
      auto py = [&](float t) { return (int16_t)(bot - (int16_t)lroundf((t - tmin) / range * (bot - top))); };
      for (uint8_t i = 0; i + 1 < n; i++) c.drawLine(px(i), py(wx.hourly[i].temp), px(i + 1), py(wx.hourly[i + 1].temp), Canvas::rgb(g_cfg.display.colors.temp));
      for (uint8_t i = 0; i < n; i++) {
        int16_t h = (int16_t)(wx.hourly[i].pop * 12 / 100);
        if (h) c.fillRect(px(i) - 1, H - h, 3, h, Canvas::color565(40, 90, 220));
        if (wx.hourly[i].hour % 6 == 0) c.drawPixel(px(i), 18, C_GREY);
      }
      c.drawFastHLine(x0, 18, x1 - x0 + 1, 0x2104);
      tinyFont(c);
      char b[8];
      snprintf(b, sizeof(b), "%d", (int)lroundf(tmax)); c.drawText(b, 0, 0, Canvas::rgb(g_cfg.display.colors.hi));
      snprintf(b, sizeof(b), "%d", (int)lroundf(tmin)); c.drawText(b, 0, 11, Canvas::rgb(g_cfg.display.colors.lo));
      c.drawText("12H", 0, 20, C_GREY);
      c.drawText("%", 0, 27, Canvas::color565(40, 90, 220));
    }
    void drawSplash(Canvas& c) {
      classicFont(c);
      c.drawTextCentered("MATRIX", W / 2, 3, 0x07FF);
      c.drawTextCentered("CLOCK", W / 2, 12, 0xFFFF);
      tinyFont(c);
      c.drawTextCentered("v" MWC_VERSION, W / 2, 24, C_GREY);
    }
    void drawOta(Canvas& c) {
      classicFont(c);
      c.drawTextCentered("UPDATE", W / 2, 3, 0xFFE0);
      c.drawRect(2, 16, W - 4, 7, C_GREY);
      c.fillRect(3, 17, (W - 6) * otaPct / 100, 5, 0x07E0);
      char p[8];
      snprintf(p, sizeof(p), "%u%%", otaPct);
      tinyFont(c);
      c.drawTextCentered(p, W / 2, 26, 0xFFFF);
    }

    uint32_t sleepAt = 0;          // millis() when the fade starts; 0 = no sleep timer
    bool sleepArmed = false;

    // Scales whatever the schedule decided. Returns 255 (no change) until the sleep moment, then ramps to 0.
    uint16_t sleepScale(uint32_t now) {
      if (!sleepArmed) return 255;
      const int32_t past = (int32_t)(now - sleepAt);
      if (past < 0) return 255;
      const uint32_t fade = (uint32_t)g_cfg.display.sleep_fade_sec * 1000UL;
      if (!fade || (uint32_t)past >= fade) return 0;
      return (uint16_t)(255 - (uint32_t)past * 255UL / fade);
    }

    uint8_t decideBrightness(const struct tm& lt, bool timeValid) {
      const DisplayConfig& d = g_cfg.display;
      uint16_t nowMin = (uint16_t)(lt.tm_hour * 60 + lt.tm_min);
      night = demo.on ? demo.night : (timeValid && d.night.enabled && in_window(d.night.start, d.night.end, nowMin));
      if (!demo.on && nightOvr) night = nightOvr == 1;
      uint8_t level = d.brightness;
      if (night) level = d.night.level;
      else if (timeValid && d.schedule.enabled) {
        bool day;
        if (d.schedule.follow_sun && wx.sunrise_min >= 0 && wx.sunset_min >= 0) {
          int start = wx.sunrise_min - d.schedule.sun_offset_min, end = wx.sunset_min + d.schedule.sun_offset_min;
          day = in_window((uint16_t)((start % 1440 + 1440) % 1440), (uint16_t)((end % 1440 + 1440) % 1440), nowMin);
        } else day = in_window(d.schedule.day_start, d.schedule.night_start, nowMin);
        level = day ? d.schedule.day_level : d.schedule.night_level;
      }
      int adj = (int)level + (int)briOffset * 16;
      if (adj < 1) adj = 1;
      if (adj > 255) adj = 255;
      level = (uint8_t)adj;
      const uint16_t sc = sleepScale(millis());
      if (sc != 255) {
        adj = (int)level * (int)sc / 255;
        return (uint8_t)(adj < 0 ? 0 : adj);      // 0 is allowed here: the sleep timer really does go dark
      }
      return level ? level : 1;
    }

    void startTransition(uint8_t fromPage, bool fromLightning, uint32_t now) {
      if (!g_cfg.display.transitions) return;
      transFrom = fromPage;
      transFromLightning = fromLightning;
      transStart = now;
    }
  }

  void begin(uint32_t now_ms) {
    radarFrame = (uint16_t*)heap_caps_calloc(RADAR_PX, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    radarBase = (uint8_t*)heap_caps_calloc(RADAR_PX, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!msgMtx) msgMtx = xSemaphoreCreateMutex();
    if (!pageA) { pageA = new Canvas(W, BOTTOM_H); pageB = new Canvas(W, BOTTOM_H); }
    screen = Screen::Splash;
    screenUntil = now_ms + SPLASH_MS;
    pageSince = now_ms;
    for (uint8_t i = 0; i < NP; i++) respawn(parts[i], true);
  }

  void applyDisplay() { lastBri = 0; if (pageIdx >= g_cfg.display.npages) pageIdx = 0; }
  void requestTest(uint32_t hold_ms) { screen = Screen::Test; testStart = millis(); screenUntil = testStart + hold_ms; }

  bool requestFullScreen(const char* name) {
    if (otaActive) return false;
    if (!strcmp(name, "radar")) { if (!radarAvailable()) return false; startRadar(millis()); return true; }
    if (!wx.valid) return false;
    if (!strcmp(name, "forecast") && wx.ndaily) screen = Screen::Forecast;
    else if (!strcmp(name, "hourly") && wx.nhourly >= 2) screen = Screen::Hourly;
    else return false;
    screenUntil = millis() + 2UL * g_cfg.display.page_sec * 1000UL;
    transFrom = 255;
    return true;
  }

  const char* fullScreenBlockReason() {
    const DisplayConfig& d = g_cfg.display;
    if (!d.forecast_page && !d.hourly_page && !g_cfg.radar.enabled) return "all full screens disabled";
    if (!wx.valid) return "no weather data yet";
    if (alarmclock::ringing()) return "alarm ringing";
    if (msg.active) return "message showing";
    if (alarmclock::timerRunning()) return "timer running";
    if (night) return "night mode";
    if (av.n && av.newest_ms && g_cfg.alerts.flash_frame_sec && millis() - av.newest_ms < (uint32_t)g_cfg.alerts.flash_frame_sec * 1000UL) return "new alert";
    return "";
  }
  void setOta(bool active, uint8_t pct) { otaActive = active; otaPct = pct; }
  void showIp(uint32_t hold_ms) { ipUntil = hold_ms ? millis() + hold_ms : 0; }

  void showMessage(const char* text, uint32_t hold_ms, uint32_t rgb) {
    if (!lockMsg()) return;
    msg.text = text ? text : "";
    msg.until = hold_ms ? millis() + hold_ms : 0;
    if (msg.until == 0 && hold_ms) msg.until = 1;
    msg.color = Canvas::rgb(rgb);
    msg.active = msg.text.length() > 0;
    unlockMsg();
  }
  void clearMessage() { if (lockMsg()) { msg.active = false; msg.text = ""; unlockMsg(); } }
  bool hasMessage() { return msg.active; }
  String messageText() { String t; if (lockMsg()) { if (msg.active) t = msg.text; unlockMsg(); } return t; }
  uint32_t messageRemainingSec() { if (!msg.active || !msg.until) return 0; int32_t d = (int32_t)(msg.until - millis()); return d > 0 ? (uint32_t)d / 1000 : 0; }

  void nextPage() {
    uint32_t now = millis();
    startTransition(g_cfg.display.pages[pageIdx < g_cfg.display.npages ? pageIdx : 0], showLightningPage, now);
    showLightningPage = false;
    pageIdx = (uint8_t)((pageIdx + 1) % (g_cfg.display.npages ? g_cfg.display.npages : 1));
    for (uint8_t g = 0; g < g_cfg.display.npages && !pageAvailable(g_cfg.display.pages[pageIdx]); g++) pageIdx = (uint8_t)((pageIdx + 1) % g_cfg.display.npages);
    pageSince = now;
    if (screen == Screen::Forecast || screen == Screen::Hourly) screen = Screen::Composite;
  }

  void setDemo(bool on, uint32_t total_ms, bool sound, uint8_t start) {
    uint32_t now = millis();
    if (on) { demo.on = true; demo.sound = sound; demo.endAt = now + (total_ms ? total_ms : 10 * 60000UL); demoApply((uint8_t)(start % DEMO_COUNT), now); demo.nextAt = now + DEMO_STEP_MS; }
    else if (demo.on) { demo.on = false; demo.soundPending = false; demo.theme = nullptr; clearMessage(); screen = Screen::Composite; transFrom = 255; lastSlow = 0; lastBri = 0; }
  }
  bool demoActive() { return demo.on; }
  bool demoSound() { return demo.on && demo.sound; }
  bool consumeDemoSound(uint8_t& style, const char*& phrase) {
    if (!demo.on || !demo.soundPending) return false;
    demo.soundPending = false;
    style = demo.soundStyle;
    phrase = demo.soundPhrase;
    return true;
  }
  const char* demoScenario() { return demo.on ? demo.name : ""; }
  uint32_t demoRemainingSec() { if (!demo.on) return 0; int32_t d = (int32_t)(demo.endAt - millis()); return d > 0 ? (uint32_t)d / 1000 : 0; }

  void adjustBrightness(int8_t steps) {
    int v = briOffset + steps;
    if (v < -12) v = -12;
    if (v > 12) v = 12;
    briOffset = (int8_t)v;
    lastBri = 0;   // re-evaluate on the next frame
  }
  int8_t brightnessOffset() { return briOffset; }
  void cycleNightOverride() { nightOvr = (uint8_t)((nightOvr + 1) % 3); lastBri = 0; }
  uint8_t nightOverride() { return nightOvr; }
  bool nightActive() { return night; }
  void startSleep(uint32_t minutes) {
    if (!minutes) { cancelSleep(); return; }
    sleepAt = millis() + minutes * 60000UL;
    sleepArmed = true;
    LOGI("display: sleep in %lu min", (unsigned long)minutes);
  }
  void cancelSleep() { if (sleepArmed) LOGI("display: sleep cancelled"); sleepArmed = false; sleepAt = 0; }
  bool sleepPending() { return sleepArmed; }
  uint32_t sleepRemainingSec() {
    if (!sleepArmed) return 0;
    const int32_t d = (int32_t)(sleepAt - millis());
    return d > 0 ? (uint32_t)(d + 999) / 1000 : 0;
  }
  bool sleepFadedOut() { return sleepArmed && sleepScale(millis()) == 0; }

  uint8_t effectiveBrightness() { return lastBri; }
  const char* themeName() { return theme ? theme->name : ""; }
  const char* screenName() {
    switch (screen) {
      case Screen::Splash: return "splash";
      case Screen::Forecast: return "forecast";
      case Screen::Hourly: return "hourly";
      case Screen::Radar: return "radar";
      case Screen::Test: return "test";
      default: return demo.on ? "demo" : alarmclock::ringing() ? "alarm" : av.n ? "alert" : msg.active ? "message" : "clock";
    }
  }

  void tick(Canvas& c, uint32_t now) {
    if (demo.on) {
      if ((int32_t)(now - demo.endAt) >= 0) setDemo(false, 0);
      else if ((int32_t)(now - demo.nextAt) >= 0) { demoApply((uint8_t)((demo.idx + 1) % DEMO_COUNT), now); demo.nextAt = now + DEMO_STEP_MS; }
    }
    if (now - lastSlow >= 500) {
      lastSlow = now;
      shared::getWeather(wx);
      alerts::view(g_cfg.alerts, av);
      ls = lightning::status();
      if (demo.on) { wx = demo.wx; av = demo.av; ls = demo.ls; }
      if (msg.active && msg.until && (int32_t)(now - msg.until) >= 0) clearMessage();
    }
    if (lightning::consumeStrikeEvent()) { strikeFlashUntil = now + 220; strikeX = 6 + random(W - 12); }
    if (demo.on && demo.lightningPage && now - demo.lastStrike > 3000) { demo.lastStrike = now; strikeFlashUntil = now + 220; strikeX = 6 + random(W - 12); }
    if (demo.on && demo.sound && demo.ringing && (demo.lastRing == 0 || now - demo.lastRing >= 6000)) {   // alarm / timer-done repeat
      demo.lastRing = now; demo.soundPending = true; demo.soundStyle = (uint8_t)ChimeStyle::TripleBeep; demo.soundPhrase = nullptr;
    }
    struct tm lt = {};
    uint16_t ms = 0;
    const bool timeValid = timesvc::localNow(lt, &ms);
    theme = demo.on ? demo.theme : ((timeValid && g_cfg.display.holiday_themes) ? themes::forDate(lt) : nullptr);

    uint8_t bri = decideBrightness(lt, timeValid);
    if (bri != lastBri) { panel::setBrightness(bri); lastBri = bri; }

    app::trace("render", screenName());
    c.fillScreen(0);
    if (otaActive) { drawOta(c); return; }
    if (screen == Screen::Test) {
      if ((int32_t)(now - screenUntil) < 0) {
        char l1[24], l2[24];
        snprintf(l1, sizeof(l1), "%ux%u %s", g_cfg.panel.width, g_cfg.panel.height, panel::driverName());
        snprintf(l2, sizeof(l2), "%dHz %luK", panel::refreshRateHz(), (unsigned long)(ESP.getFreeHeap() / 1024));
        test_pattern::draw(c, now - testStart, l1, l2);
        return;
      }
      screen = Screen::Composite;
    }
    if (screen == Screen::Splash) {
      if ((int32_t)(now - screenUntil) < 0) { drawSplash(c); return; }
      screen = Screen::Composite;
    }

    const DisplayConfig& d = g_cfg.display;
    const bool ringing = alarmclock::ringing() || (demo.on && demo.ringing);
    const bool timerRun = alarmclock::timerRunning() || (demo.on && demo.timer);
    const bool alert = av.n > 0;
    const bool alertFresh = alert && av.newest_ms && g_cfg.alerts.flash_frame_sec && now - av.newest_ms < (uint32_t)g_cfg.alerts.flash_frame_sec * 1000UL;
    const bool interrupt = ringing || alertFresh || msg.active || timerRun;   // blocks the full screens

    // an active full screen (forecast / hourly graph) owns the panel until its time is up
    if (screen == Screen::Forecast || screen == Screen::Hourly || screen == Screen::Radar) {
      if ((int32_t)(now - screenUntil) < 0 && !interrupt) {
        if (screen == Screen::Forecast) drawForecast(c); else if (screen == Screen::Hourly) drawHourly(c); else drawRadar(c, now);
        return;
      }
      screen = Screen::Composite;
      pageSince = now;
      transFrom = 255;
    }

    // page rotation (and the full-screen forecast / hourly graph every few cycles); demo mode pins its own page
    if (!demo.on && now - pageSince >= (uint32_t)d.page_sec * 1000UL) {
      pageSince = now;
      if (ls.active && !showLightningPage && !lightningTurn) {
        startTransition(d.pages[pageIdx < d.npages ? pageIdx : 0], false, now);
        showLightningPage = true; lightningTurn = true;
      } else {
        startTransition(d.pages[pageIdx < d.npages ? pageIdx : 0], showLightningPage, now);
        if (showLightningPage) { showLightningPage = false; }
        else {
          lightningTurn = false;
          pageIdx++;
          // pages whose data is missing (sensor unplugged, no tide station) are skipped without breaking the cycle count
          while (pageIdx < d.npages && !pageAvailable(d.pages[pageIdx])) pageIdx++;
          if (pageIdx >= d.npages) {
            pageIdx = 0;
            cyclesSinceFull++;
            cyclesSinceRadar++;
            const bool radarOk = !interrupt && !night && radarAvailable();
            const bool precip = radarOk && g_cfg.radar.show_when_precip && (radar::echoNearby() || (wx.valid && isPrecipCode(wx.cur.wmo)));
            if (precip && cyclesSinceRadar >= g_cfg.radar.precip_every_n_cycles) {
              // rain or snow around: the radar loop comes back every few page cycles on its own
              cyclesSinceRadar = 0;
              startRadar(now);
            } else if (!interrupt && !night && cyclesSinceFull >= d.forecast_every_n_cycles) {
              // round-robin over the enabled full screens: forecast, hourly graph, radar
              Screen cand[3]; uint8_t nc = 0;
              if (d.forecast_page && wx.valid && wx.ndaily) cand[nc++] = Screen::Forecast;
              if (d.hourly_page && wx.valid && wx.nhourly >= 2) cand[nc++] = Screen::Hourly;
              if (radarOk && !precip && g_cfg.radar.every_n_cycles && (cyclesSinceRadar + 1) >= g_cfg.radar.every_n_cycles / max<uint8_t>(1, d.forecast_every_n_cycles)) cand[nc++] = Screen::Radar;
              if (nc) {
                cyclesSinceFull = 0;
                Screen pick = cand[fullTurn % nc];
                fullTurn++;
                if (pick == Screen::Radar) { cyclesSinceRadar = 0; startRadar(now); }
                else { screen = pick; screenUntil = now + 2UL * d.page_sec * 1000UL; transFrom = 255; }
              }
            }
          }
        }
      }
    }
    if (screen == Screen::Forecast || screen == Screen::Hourly || screen == Screen::Radar) {   // just switched to a full screen this tick
      if (screen == Screen::Forecast) drawForecast(c); else if (screen == Screen::Hourly) drawHourly(c); else drawRadar(c, now);
      return;
    }

    drawClock(c, lt, timeValid, ms);

    // effects behind the bottom half (weather pages only)
    Fx fx = Fx::None;
    if (d.precip_fx && wx.valid && !night && !interrupt) {
      if (isThunderCode(wx.cur.wmo)) fx = Fx::Thunder;
      else if (isRainCode(wx.cur.wmo)) fx = Fx::Rain;
      else if (isSnowCode(wx.cur.wmo)) fx = Fx::Snow;
    }
    if (fx == Fx::None && theme && !night && !interrupt) {
      switch (theme->deco) {
        case themes::Deco::Snow: fx = Fx::Snow; break;
        case themes::Deco::Confetti: fx = Fx::Confetti; break;
        case themes::Deco::Hearts: fx = Fx::Hearts; break;
        case themes::Deco::Sparkle: fx = Fx::Sparkle; break;
        default: break;
      }
    }
    fxSelect(fx);
    fxDraw(c, now);

    Canvas& pc = *pageB;
    pc.fillScreen(0);
    bool slide = false;
    if (ringing) drawRing(pc, now, demo.on && demo.ringing ? demo.ringTimer : alarmclock::ringingIsTimer(), demo.on && demo.ringing ? "DEMO" : alarmclock::ringingLabel());
    else if (alert) drawBanner(pc, now);
    else if (msg.active) drawMessage(pc, now);
    else if (timerRun) drawTimer(pc, demo.on && demo.timer ? (uint32_t)max<int32_t>(0, (int32_t)(demo.timerEnd - now)) / 1000 : alarmclock::timerRemainingSec());
    else if (!demo.on && alarmclock::stopwatchActive()) drawStopwatch(pc);
    else if (!demo.on && ipUntil && (int32_t)(now - ipUntil) < 0 && wifi_mgr::isConnected()) drawIp(pc);
    else if (!timeValid && !demo.on) drawStatusLine(pc);
    else if (night && d.night.hide_bottom) { /* dark */ }
    else {
      if ((demo.on && demo.lightningPage) || (showLightningPage && ls.active)) drawLightningPage(pc);
      else drawPage(pc, demo.on && demo.page != 255 ? demo.page : d.pages[pageIdx < d.npages ? pageIdx : 0], lt, timeValid, now);
      slide = transFrom != 255 && (int32_t)(now - (transStart + TRANS_MS)) < 0;
    }
    if (slide) {
      Canvas& po = *pageA;
      po.fillScreen(0);
      if (transFromLightning) drawLightningPage(po); else drawPage(po, transFrom, lt, timeValid, now);
      int16_t dx = (int16_t)(-(int32_t)W * (int32_t)(now - transStart) / (int32_t)TRANS_MS);
      blitBottom(c, po, dx);
      blitBottom(c, pc, W + dx);
    } else {
      transFrom = 255;
      blitBottom(c, pc, 0);
    }

    // frame flashes: alarm (orange), new alert (severity colour), lightning strike (bolt across the panel)
    if (ringing && ((now / 500) & 1)) c.drawRect(0, 0, W, H, C_ORANGE);
    else if (alert && av.newest_ms && g_cfg.alerts.flash_frame_sec && now - av.newest_ms < (uint32_t)g_cfg.alerts.flash_frame_sec * 1000UL && ((now / FRAME_FLASH_MS) & 1))
      c.drawRect(0, 0, W, H, sevColor(av.top));
    if ((int32_t)(now - strikeFlashUntil) < 0 && g_cfg.lightning.show_bolt) drawBolt(c, strikeX, 0, H - 1, ((now / 40) & 1) ? 0xFFFF : C_BOLT);
  }
}
