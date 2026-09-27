#include "weather_client.h"
#include "http_util.h"
#include "util/psram_alloc.h"
#include "util/log.h"
#include "version.h"

namespace weather_client {
  String buildUrl(const AppConfig& cfg) {
    String url;
    url.reserve(400);
    url += "http://api.open-meteo.com/v1/forecast?latitude=";
    url += String(cfg.location.lat, 4);
    url += "&longitude=";
    url += String(cfg.location.lon, 4);
    url += "&current=temperature_2m,relative_humidity_2m,apparent_temperature,is_day,weather_code,wind_speed_10m,wind_direction_10m,wind_gusts_10m";
    url += ",uv_index,cloud_cover,visibility,precipitation";
    url += "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,sunrise,sunset";
    url += ",precipitation_sum,uv_index_max";
    url += "&hourly=temperature_2m,precipitation_probability&forecast_hours=12";
    if (cfg.weather.imperial) url += "&temperature_unit=fahrenheit&wind_speed_unit=mph&precipitation_unit=inch";
    else url += "&wind_speed_unit=kmh";
    url += "&timezone=auto&forecast_days=";
    url += String(constrain(cfg.weather.forecast_days, 1, 3));
    return url;
  }

  bool fetch(const AppConfig& cfg, WeatherData& out, String& err) {
    JsonDocument doc(psramAllocator());
    JsonDocument filter;
    filter["current"] = true;
    filter["elevation"] = true;
    filter["daily"] = true;
    filter["hourly"] = true;
    filter["utc_offset_seconds"] = true;
    http_util::Options opt;
    opt.userAgent = MWC_USER_AGENT_NAME "/" MWC_VERSION;
    opt.accept = "application/json";
    bool ok = http_util::get(buildUrl(cfg), opt, [&](Stream& s, int) {
      DeserializationError e = deserializeJson(doc, s, DeserializationOption::Filter(filter));
      if (e) { err = String("json: ") + e.c_str(); return false; }
      return true;
    }, err);
    if (!ok) return false;

    JsonObject cur = doc["current"];
    JsonObject daily = doc["daily"];
    if (cur.isNull() || !cur["temperature_2m"].is<float>()) { err = "json: missing current"; return false; }
    WeatherData w;
    w.valid = true;
    w.imperial = cfg.weather.imperial;
    w.fetched_ms = millis();
    w.utc_offset_s = doc["utc_offset_seconds"] | 0;
    w.elevation_m = doc["elevation"] | -9999.0f;
    w.cur.temp = cur["temperature_2m"] | 0.0f;
    w.cur.feels = cur["apparent_temperature"] | w.cur.temp;
    w.cur.humidity = cur["relative_humidity_2m"] | 0.0f;
    w.cur.wind = cur["wind_speed_10m"] | 0.0f;
    w.cur.gust = cur["wind_gusts_10m"] | 0.0f;
    w.cur.wind_dir = cur["wind_direction_10m"] | 0;
    w.cur.wmo = cur["weather_code"] | 0;
    w.cur.is_day = (cur["is_day"] | 1) != 0;
    w.cur.uv = cur["uv_index"] | -1.0f;
    w.cur.cloud = cur["cloud_cover"] | -1.0f;
    w.cur.rain = cur["precipitation"] | -1.0f;
    {   // visibility follows the unit set: feet with precipitation_unit=inch, metres otherwise
      float v = cur["visibility"] | -1.0f;
      w.cur.vis = v < 0 ? -1.0f : (cfg.weather.imperial ? v / 5280.0f : v / 1000.0f);
    }
    if (!daily.isNull()) {
      JsonArray t = daily["time"], wc = daily["weather_code"], tmax = daily["temperature_2m_max"],
                tmin = daily["temperature_2m_min"], pop = daily["precipitation_probability_max"],
                rsum = daily["precipitation_sum"], uvm = daily["uv_index_max"];
      for (size_t i = 0; i < t.size() && i < 3; i++) {
        WeatherDaily& d = w.daily[i];
        strlcpy(d.date, t[i] | "", sizeof(d.date));
        d.wmo = wc[i] | 0;
        d.tmax = tmax[i] | 0.0f;
        d.tmin = tmin[i] | 0.0f;
        d.pop = pop[i] | 0;
        d.rain_sum = rsum[i] | -1.0f;
        d.uv_max = uvm[i] | -1.0f;
        w.ndaily = i + 1;
      }
      const char* sr = daily["sunrise"][0] | "";
      const char* ss = daily["sunset"][0] | "";
      int h, m;
      if (strlen(sr) >= 16 && sscanf(sr + 11, "%2d:%2d", &h, &m) == 2) w.sunrise_min = (int16_t)(h * 60 + m);
      if (strlen(ss) >= 16 && sscanf(ss + 11, "%2d:%2d", &h, &m) == 2) w.sunset_min = (int16_t)(h * 60 + m);
    }
    JsonObject hourly = doc["hourly"];
    if (!hourly.isNull()) {
      JsonArray t = hourly["time"], temp = hourly["temperature_2m"], pop = hourly["precipitation_probability"];
      for (size_t i = 0; i < t.size() && i < HOURLY_COUNT; i++) {
        const char* ts = t[i] | "";
        w.hourly[i].hour = strlen(ts) >= 13 ? (int8_t)atoi(ts + 11) : 0;
        w.hourly[i].temp = temp[i] | 0.0f;
        w.hourly[i].pop = pop[i] | 0;
        w.nhourly = i + 1;
      }
    }
    out = w;
    LOGI("weather: %.1f%s wmo %u, %u days", w.cur.temp, w.imperial ? "F" : "C", w.cur.wmo, w.ndaily);
    return true;
  }
}
