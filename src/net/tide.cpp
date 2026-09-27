#include "tide.h"
#include <math.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "http_util.h"
#include "net_task.h"
#include "wifi_manager.h"
#include "time/time_service.h"
#include "util/psram_alloc.h"
#include "util/log.h"
#include "version.h"

namespace tide {
  namespace {
    constexpr const char* API = "https://api.tidesandcurrents.noaa.gov/api/prod/datagetter?product=predictions&datum=MLLW&time_zone=lst_ldt&interval=hilo&format=json";
    constexpr const char* API_WTEMP = "https://api.tidesandcurrents.noaa.gov/api/prod/datagetter?product=water_temperature&date=latest&time_zone=lst_ldt&format=json";
    constexpr uint32_t FIRST_MS = 25000UL, RETRY_MS = 15 * 60000UL;
    SemaphoreHandle_t mtx = nullptr;
    Data data;
    Status st = {};
    uint32_t nextFetch = 0;
    volatile bool refreshReq = false, resetReq = false;

    bool take() { return mtx && xSemaphoreTake(mtx, pdMS_TO_TICKS(100)) == pdTRUE; }
    void give() { xSemaphoreGive(mtx); }

    bool parseLocal(const char* s, time_t& out) {   // "YYYY-MM-DD HH:MM" in the station's local time = the clock's zone
      int Y, M, D, h, m;
      if (sscanf(s, "%d-%d-%d %d:%d", &Y, &M, &D, &h, &m) != 5) return false;
      struct tm t = {};
      t.tm_year = Y - 1900; t.tm_mon = M - 1; t.tm_mday = D; t.tm_hour = h; t.tm_min = m; t.tm_isdst = -1;
      timesvc::tzLock();
      out = mktime(&t);
      timesvc::tzUnlock();
      return out > 0;
    }

    // Not every station carries a thermometer, so a miss here is silent and leaves the previous reading alone.
    void fetchWaterTemp(const AppConfig& cfg) {
      const bool metric = cfg.tide.unit == 2 || (cfg.tide.unit == 0 && !cfg.weather.imperial);
      String url = String(API_WTEMP) + "&station=" + cfg.tide.station + "&units=" + (metric ? "metric" : "english");
      http_util::Options opt;
      opt.userAgent = MWC_USER_AGENT_NAME "/" MWC_VERSION;
      opt.accept = "application/json";
      opt.timeoutMs = 15000;
      JsonDocument doc(psramAllocator());
      String err;
      if (!http_util::get(url, opt, [&](Stream& s, int) { return deserializeJson(doc, s) == DeserializationError::Ok; }, err)) return;
      if (!doc["error"].isNull()) return;
      JsonArray a = doc["data"];
      if (a.isNull() || a.size() == 0) return;
      const char* v = a[a.size() - 1]["v"] | "";
      if (!v[0]) return;
      float t = atof(v);
      if (t < -10 || t > 150) return;
      time_t when = 0;
      parseLocal(a[a.size() - 1]["t"] | "", when);
      if (take()) { data.water_temp = t; data.water_t = when; give(); }
      LOGI("tide: water %.1f%s at station %s", t, metric ? "C" : "F", cfg.tide.station);
    }

    bool fetch(const AppConfig& cfg) {
      const bool metric = cfg.tide.unit == 2 || (cfg.tide.unit == 0 && !cfg.weather.imperial);
      time_t nowT = time(nullptr);
      struct tm lt; timesvc::tzLock(); localtime_r(&nowT, &lt); timesvc::tzUnlock();
      char begin[12]; strftime(begin, sizeof(begin), "%Y%m%d", &lt);
      String url = String(API) + "&station=" + cfg.tide.station + "&units=" + (metric ? "metric" : "english") + "&begin_date=" + begin + "&range=54";
      http_util::Options opt;
      opt.userAgent = MWC_USER_AGENT_NAME "/" MWC_VERSION;
      opt.accept = "application/json";
      opt.timeoutMs = 20000;
      JsonDocument doc(psramAllocator());
      String err;
      int code = 0;
      bool ok = http_util::get(url, opt, [&](Stream& s, int) { return deserializeJson(doc, s) == DeserializationError::Ok; }, err, &code);
      if (!ok) { if (take()) { st.last_err_ms = millis(); if (st.fails < 10) st.fails++; strlcpy(st.err, err.c_str(), sizeof(st.err)); give(); } LOGW("tide: %s", err.c_str()); return false; }
      if (!doc["error"].isNull()) {
        const char* m = doc["error"]["message"] | "station error";
        if (take()) { st.last_err_ms = millis(); if (st.fails < 10) st.fails++; strlcpy(st.err, m, sizeof(st.err)); give(); }
        LOGW("tide: %s", m);
        return false;
      }
      Data d;
      d.metric = metric;
      strlcpy(d.station, cfg.tide.station, sizeof(d.station));
      for (JsonObject p : doc["predictions"].as<JsonArray>()) {
        if (d.n >= MAX_EXTREMES) break;
        time_t t;
        if (!parseLocal(p["t"] | "", t)) continue;
        const char* ty = p["type"] | "";
        d.ex[d.n].t = t;
        d.ex[d.n].h = atof(p["v"] | "0");
        d.ex[d.n].high = ty[0] == 'H';
        d.n++;
      }
      if (d.n < 2) { if (take()) { st.last_err_ms = millis(); strlcpy(st.err, "no predictions", sizeof(st.err)); give(); } LOGW("tide: no predictions for station %s", cfg.tide.station); return false; }
      d.valid = true;
      d.fetched_ms = millis();
      if (take()) { d.water_temp = data.water_temp; d.water_t = data.water_t; data = d; st.valid = true; st.last_ok_ms = millis(); st.fails = 0; st.err[0] = '\0'; give(); }
      LOGI("tide: station %s, %u extremes, next %s at %ld", d.station, d.n, d.ex[0].high ? "high" : "low", (long)d.ex[0].t);
      return true;
    }
  }

  void begin() { mtx = xSemaphoreCreateMutex(); nextFetch = millis() + FIRST_MS; }
  void applyConfig() { resetReq = true; nextFetch = 0; net_task::kick(net_task::JOB_TIDE); }
  void requestRefresh() { refreshReq = true; net_task::kick(net_task::JOB_TIDE); }

  bool due(const AppConfig& cfg, uint32_t now) {
    if (!cfg.tide.enabled || !cfg.tide.station[0]) return false;
    if (refreshReq || resetReq) return true;
    if ((int32_t)(now - nextFetch) >= 0) return true;
    // keep at least six hours of future extremes on hand
    Data d; get(d);
    if (d.valid && d.n && d.ex[d.n - 1].t - time(nullptr) < 6 * 3600) return true;
    return false;
  }

  void run(const AppConfig& cfg) {
    st.enabled = cfg.tide.enabled && cfg.tide.station[0];
    if (resetReq) { resetReq = false; if (take()) { data = Data(); st.valid = false; give(); } }
    if (!st.enabled || !wifi_mgr::isConnected()) return;
    if (time(nullptr) < 1700000000) { nextFetch = millis() + 30000UL; return; }   // the request is dated in local time
    refreshReq = false;
    const uint32_t now = millis();
    const bool ok = fetch(cfg);
    if (cfg.tide.water_temp) fetchWaterTemp(cfg);
    if (ok) nextFetch = now + (uint32_t)(cfg.tide.refresh_hours ? cfg.tide.refresh_hours : 6) * 3600000UL;
    else nextFetch = now + RETRY_MS;
  }

  Status status() { Status c = {}; if (take()) { c = st; give(); } return c; }
  bool get(Data& out) { if (!take()) return false; out = data; give(); return out.valid; }

  bool now(const Data& d, time_t t, Extreme& nextHigh, Extreme& nextLow, bool& rising, float& height) {
    if (!d.valid || d.n < 2) return false;
    int after = -1;
    for (uint8_t i = 0; i < d.n; i++) if (d.ex[i].t > t) { after = i; break; }
    if (after < 0) return false;
    bool haveH = false, haveL = false;
    for (uint8_t i = (uint8_t)after; i < d.n && !(haveH && haveL); i++) {
      if (d.ex[i].high && !haveH) { nextHigh = d.ex[i]; haveH = true; }
      if (!d.ex[i].high && !haveL) { nextLow = d.ex[i]; haveL = true; }
    }
    if (!haveH || !haveL) return false;
    rising = d.ex[after].high;
    if (after == 0) { height = d.ex[0].h; return true; }
    const Extreme& a = d.ex[after - 1];
    const Extreme& b = d.ex[after];
    float f = (float)(t - a.t) / (float)(b.t - a.t);
    height = a.h + (b.h - a.h) * (1.0f - cosf(f * (float)M_PI)) / 2.0f;
    return true;
  }
}
