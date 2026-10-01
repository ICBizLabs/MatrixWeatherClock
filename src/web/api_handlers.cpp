// REST API: /api/* endpoints. Handlers run on the async_tcp task: they only copy state under mutexes and stage
// changes for the main loop; they never touch the network, I2C or the renderer directly.
#include "web_server.h"
#include <AsyncJson.h>
#include <esp_heap_caps.h>
#include "app.h"
#include "version.h"
#include "config/config.h"
#include "display/panel.h"
#include "display/renderer.h"
#include "display/themes.h"
#include "display/wmo.h"
#include "net/wifi_manager.h"
#include "net/net_task.h"
#include "net/shared_state.h"
#include "net/alert_store.h"
#include "audio/audio_out.h"
#include "audio/voice.h"
#include "net/voice_pack.h"
#include "net/radar.h"
#include "net/tide.h"
#include "net/webhook.h"
#include "util/moon.h"
#include "util/zambretti.h"
#include "io/i2c_bus.h"
#include "io/buttons.h"
#include "io/env_sensor.h"
#include "io/ir_remote.h"
#include "io/actions.h"
#include "time/time_service.h"
#include "time/tz_table.h"
#include "alarm/alarm.h"
#include "net/lightning.h"
#include "net/pushbullet.h"
#include "net/updater.h"
#include "display/frame_snapshot.h"
#include "util/log.h"

namespace web {
  namespace {
    uint32_t ageSec(uint32_t stamp_ms) { return stamp_ms ? (millis() - stamp_ms) / 1000 : 0; }

    void weatherToJson(JsonObject o, const WeatherData& w) {
      o["valid"] = w.valid;
      if (!w.valid) return;
      o["units"] = w.imperial ? "imperial" : "metric";
      o["age_s"] = ageSec(w.fetched_ms);
      o["utc_offset_s"] = w.utc_offset_s;
      JsonObject c = o["cur"].to<JsonObject>();
      c["temp"] = w.cur.temp; c["feels"] = w.cur.feels; c["humidity"] = w.cur.humidity;
      c["wind"] = w.cur.wind; c["gust"] = w.cur.gust; c["wind_dir"] = w.cur.wind_dir;
      c["wmo"] = w.cur.wmo; c["text"] = wmo::text(w.cur.wmo); c["is_day"] = w.cur.is_day;
      if (w.cur.uv >= 0) c["uv"] = serialized(String(w.cur.uv, 1));
      if (w.cur.cloud >= 0) c["cloud"] = (int)lroundf(w.cur.cloud);
      if (w.cur.vis >= 0) c["visibility"] = serialized(String(w.cur.vis, 1));
      if (w.cur.rain >= 0) c["precip"] = serialized(String(w.cur.rain, 2));
      if (w.cur.pressure > 0) {
        c["pressure"] = serialized(String(w.cur.pressure, 1));
        c["d_press_3h"] = serialized(String(w.cur.d_press_3h, 1));
        c["press_span_min"] = w.cur.press_span_min;
      }
      JsonArray d = o["daily"].to<JsonArray>();
      for (uint8_t i = 0; i < w.ndaily; i++) {
        JsonObject x = d.add<JsonObject>();
        x["date"] = w.daily[i].date; x["wmo"] = w.daily[i].wmo; x["text"] = wmo::text(w.daily[i].wmo);
        x["tmax"] = w.daily[i].tmax; x["tmin"] = w.daily[i].tmin; x["pop"] = w.daily[i].pop;
        if (w.daily[i].rain_sum >= 0) x["rain_sum"] = serialized(String(w.daily[i].rain_sum, 2));
        if (w.daily[i].uv_max >= 0) x["uv_max"] = serialized(String(w.daily[i].uv_max, 1));
      }
    }

    void handleStatus(AsyncWebServerRequest* r) {
      AsyncJsonResponse* res = new AsyncJsonResponse();
      JsonObject root = res->getRoot().to<JsonObject>();
      root["version"] = MWC_VERSION;
      root["uptime_s"] = app::uptimeSec();

      JsonObject t = root["time"].to<JsonObject>();
      struct tm lt; uint16_t ms;
      bool tv = timesvc::localNow(lt, &ms);
      t["valid"] = tv;
      if (tv) { char b[32]; strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S", &lt); t["local"] = b; }
      timesvc::Status ts = timesvc::status();
      t["source"] = timesvc::sourceName(ts.source);
      t["last_sync"] = (long)ts.last_sync;
      t["rtc_present"] = ts.rtc_present;
      app::cfgLock();
      t["tz"] = g_cfg.time.tz_posix;
      AlertsConfig ac = g_cfg.alerts;
      app::cfgUnlock();

      WeatherData w;
      shared::getWeather(w);
      weatherToJson(root["weather"].to<JsonObject>(), w);

      JsonObject al = root["alerts"].to<JsonObject>();
      al["count"] = alerts::count();
      al["stale"] = alerts::stale();
      alerts::toJson(al["items"].to<JsonArray>(), ac, false);

      JsonObject wi = root["wifi"].to<JsonObject>();
      wi["connected"] = wifi_mgr::isConnected();
      wi["ap"] = wifi_mgr::apActive();
      wi["ap_ssid"] = wifi_mgr::apSsid();
      wi["ip"] = wifi_mgr::ip().toString();
      wi["rssi"] = wifi_mgr::rssi();

      NetStatus ns;
      shared::getNet(ns);
      JsonObject n = root["net"].to<JsonObject>();
      n["wx_ok_age_s"] = ageSec(ns.last_wx_ok); n["wx_err_age_s"] = ageSec(ns.last_wx_err); n["wx_err"] = ns.wx_err; n["wx_fails"] = ns.wx_fails;
      n["al_ok_age_s"] = ageSec(ns.last_al_ok); n["al_err_age_s"] = ageSec(ns.last_al_err); n["al_err"] = ns.al_err; n["al_fails"] = ns.al_fails;

      app::trace("web", "status");
      JsonObject sys = root["sys"].to<JsonObject>();
      sys["heap_free"] = ESP.getFreeHeap();
      sys["heap_largest"] = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      sys["psram_free"] = ESP.getFreePsram();
      sys["refresh_hz"] = panel::refreshRateHz();
      sys["panel_ok"] = panel::valid();
      sys["driver"] = panel::driverName();
      sys["screen"] = renderer::screenName();
      sys["brightness"] = renderer::effectiveBrightness();
      sys["night"] = renderer::nightActive();
      sys["reboot_required"] = app::rebootRequiredFlags() != 0;
      sys["brightness_offset"] = renderer::brightnessOffset();
      sys["night_override"] = renderer::nightOverride() == 1 ? "on" : renderer::nightOverride() == 2 ? "off" : "auto";
      {
        ir_remote::Status rs = ir_remote::status();
        JsonObject rm = root["remote"].to<JsonObject>();
        rm["enabled"] = rs.enabled; rm["pin"] = rs.pin; rm["received"] = rs.received; rm["learning"] = rs.learning;
        char hex[12]; snprintf(hex, sizeof(hex), "0x%08lX", (unsigned long)rs.last_code);
        rm["last_code"] = rs.last_code ? hex : "";
        rm["last_proto"] = rs.last_proto;
        rm["last_age_s"] = rs.last_ms ? (long)((millis() - rs.last_ms) / 1000) : -1L;
        rm["last_ms"] = rs.last_ms;
      }
      {
        app::LastReset lr = app::lastReset();
        JsonObject rs = sys["last_reset"].to<JsonObject>();
        rs["reason"] = app::resetReasonName(lr.reason);
        rs["crash"] = lr.valid;
        if (lr.valid) { rs["where"] = lr.where; rs["uptime_s"] = lr.uptime_s; rs["heap"] = lr.heap; }
      }
      JsonObject am = root["alarm"].to<JsonObject>();
      am["ringing"] = alarmclock::ringing();
      am["ringing_timer"] = alarmclock::ringingIsTimer();
      am["label"] = alarmclock::ringingLabel();
      am["snoozed"] = alarmclock::snoozed();
      am["snooze_remaining_s"] = alarmclock::snoozeRemainingSec();
      am["timer_running"] = alarmclock::timerRunning();
      am["timer_remaining_s"] = alarmclock::timerRemainingSec();
      am["striking"] = alarmclock::striking();
      JsonObject sw = root["stopwatch"].to<JsonObject>();
      sw["running"] = alarmclock::stopwatchRunning();
      sw["active"] = alarmclock::stopwatchActive();
      sw["ms"] = alarmclock::stopwatchMs();
      JsonObject sl = root["sleep"].to<JsonObject>();
      sl["pending"] = renderer::sleepPending();
      sl["remaining_s"] = renderer::sleepRemainingSec();
      sl["faded"] = renderer::sleepFadedOut();
      JsonObject mg = root["message"].to<JsonObject>();
      mg["active"] = renderer::hasMessage();
      mg["text"] = renderer::messageText();
      mg["remaining_s"] = renderer::messageRemainingSec();
      lightning::Status lst = lightning::status();
      JsonObject lg = root["lightning"].to<JsonObject>();
      lg["enabled"] = lst.enabled; lg["connected"] = lst.connected; lg["active"] = lst.active;
      lg["count"] = lst.count; lg["nearest_km"] = lst.nearest_km; lg["latest_km"] = lst.latest_km;
      lg["latest_bearing"] = lst.latest_bearing; lg["latest_age_s"] = lst.latest_time ? (long)(time(nullptr) - lst.latest_time) : -1;
      lg["total"] = lst.total;
      sys["theme"] = renderer::themeName();
      sys["theme_forced"] = g_cfg.display.force_theme[0] != '\0';
      sys["full_screen_block"] = renderer::fullScreenBlockReason();
      JsonObject dm = root["demo"].to<JsonObject>();
      dm["on"] = renderer::demoActive();
      dm["scenario"] = renderer::demoScenario();
      dm["sound"] = renderer::demoSound();
      dm["remaining_s"] = renderer::demoRemainingSec();
      updater::Status us = updater::status();
      JsonObject up = root["update"].to<JsonObject>();
      up["state"] = updater::stateName(us.state);
      up["current"] = MWC_VERSION;
      up["latest"] = us.latest;
      up["file"] = us.file;
      up["size"] = us.size;
      up["progress"] = us.progress;
      up["last_check_age_s"] = us.last_check_ms ? (long)((millis() - us.last_check_ms) / 1000) : -1L;
      up["error"] = us.err;
      pushbullet::Status pbs = pushbullet::status();
      JsonObject pb = root["pushbullet"].to<JsonObject>();
      pb["configured"] = pbs.configured; pb["device_ok"] = pbs.device_ok; pb["sent"] = pbs.sent; pb["received"] = pbs.received;
      pb["errors"] = pbs.errors; pb["last_err"] = pbs.last_err; pb["last_poll_age_s"] = pbs.last_poll_ms ? (millis() - pbs.last_poll_ms) / 1000 : 0;
      JsonObject au = root["audio"].to<JsonObject>();
      au["available"] = audio_out::available();
      app::cfgLock(); au["enabled"] = g_cfg.audio.enabled; app::cfgUnlock();
      au["quiet_now"] = audio_out::inQuietHours();
      au["buttons"] = buttons::available();
      voice::PackInfo pi = voice::info();
      voice_pack::Status vps = voice_pack::status();
      JsonObject sp = root["speech"].to<JsonObject>();
      app::cfgLock();
      sp["enabled"] = g_cfg.audio.speech.enabled;
      app::cfgUnlock();
      sp["installed"] = pi.installed;
      sp["version"] = pi.version;
      sp["format"] = pi.format;
      sp["voice"] = pi.voice;
      sp["clips"] = pi.clips;
      sp["state"] = voice_pack::stateName(vps.state);
      sp["progress"] = vps.progress;
      sp["available"] = vps.available_version;
      sp["error"] = vps.err;
      sp["can_say_time"] = voice::canSayTime();
      {
        webhook::Status ws = webhook::status();
        JsonObject wh = root["webhook"].to<JsonObject>();
        wh["configured"] = ws.configured;
        wh["sent"] = ws.sent;
        wh["errors"] = ws.errors;
        wh["error"] = ws.last_err;
        wh["last_age_s"] = ws.last_ms ? (long)((millis() - ws.last_ms) / 1000) : -1L;
      }
      {
        moon::Info mi = moon::at(time(nullptr));
        JsonObject mo = root["moon"].to<JsonObject>();
        mo["phase"] = moon::name(mi.phase);
        mo["illumination"] = (int)lroundf(mi.illumination * 100);
        mo["age_days"] = serialized(String(mi.age_days, 1));
        mo["waxing"] = mi.waxing;
        mo["days_to_full"] = serialized(String(mi.days_to_full, 1));
        mo["days_to_new"] = serialized(String(mi.days_to_new, 1));
      }
      {
        tide::Status ts = tide::status();
        tide::Data td; tide::get(td);
        JsonObject to = root["tide"].to<JsonObject>();
        to["enabled"] = ts.enabled;
        to["valid"] = td.valid;
        to["station"] = td.station;
        to["unit"] = td.metric ? "m" : "ft";
        to["last_ok_age_s"] = ts.last_ok_ms ? (long)((millis() - ts.last_ok_ms) / 1000) : -1L;
        if (td.water_temp > -999) { to["water_temp"] = serialized(String(td.water_temp, 1)); to["water_t"] = (long)td.water_t; }
        to["error"] = ts.err;
        tide::Extreme hi, lo; bool rising = false; float h = 0;
        if (td.valid && tide::now(td, time(nullptr), hi, lo, rising, h)) {
          to["rising"] = rising;
          to["height"] = serialized(String(h, 2));
          JsonObject nh = to["next_high"].to<JsonObject>(); nh["t"] = (long)hi.t; nh["h"] = serialized(String(hi.h, 2));
          JsonObject nl = to["next_low"].to<JsonObject>(); nl["t"] = (long)lo.t; nl["h"] = serialized(String(lo.h, 2));
        }
        JsonArray all = to["extremes"].to<JsonArray>();
        for (uint8_t i = 0; i < td.n; i++) { JsonObject e = all.add<JsonObject>(); e["t"] = (long)td.ex[i].t; e["h"] = serialized(String(td.ex[i].h, 2)); e["high"] = td.ex[i].high; }
      }
      {
        radar::Status rs = radar::status();
        JsonObject rd = root["radar"].to<JsonObject>();
        rd["enabled"] = rs.enabled;
        rd["frames"] = rs.frames;
        rd["last_ok_age_s"] = rs.last_ok_ms ? (long)((millis() - rs.last_ok_ms) / 1000) : -1L;
        rd["error"] = rs.err;
        rd["fails"] = rs.fails;
        rd["echo_near"] = rs.echo_near;
        rd["echo_pct"] = rs.echo_pct;
        rd["newest_age_min"] = rs.frames ? radar::frameAgeMin(rs.frames - 1) : -1;
        rd["base"] = rs.base_state == 2 ? "ready" : rs.base_state == 1 ? "loading" : rs.base_state == 3 ? "error" : "off";
      }
      {
        env_sensor::Reading er = env_sensor::reading();
        JsonObject in = root["indoor"].to<JsonObject>();
        in["present"] = env_sensor::present();
        in["sensor"] = env_sensor::typeName();
        in["address"] = env_sensor::address();
        in["valid"] = er.valid;
        if (er.valid) {
          in["temp_c"] = serialized(String(er.temp_c, 1));
          in["temp_f"] = serialized(String(er.temp_c * 9.0f / 5.0f + 32.0f, 1));
          in["has_humidity"] = er.has_humidity;
          in["humidity"] = serialized(String(er.humidity, 1));
          in["pressure_hpa"] = serialized(String(er.pressure_hpa, 1));
          in["sea_level_hpa"] = serialized(String(er.sea_level_hpa, 1));
          in["sea_level_known"] = er.sea_level_known;
          in["altitude_m"] = serialized(String(er.altitude_m, 0));
          in["trend_temp"] = env_sensor::trendName(er.t_temp);
          in["trend_humidity"] = env_sensor::trendName(er.t_hum);
          in["trend_pressure"] = env_sensor::trendName(er.t_press);
          in["d_temp_c"] = serialized(String(er.d_temp, 2));
          in["d_humidity"] = serialized(String(er.d_hum, 1));
          in["d_pressure_hpa"] = serialized(String(er.d_press, 2));
          in["span_min"] = er.span_min;
          in["age_s"] = (millis() - er.sample_ms) / 1000;
          in["has_gas"] = er.has_gas;
          if (er.has_gas) {
            in["gas_valid"] = er.gas_valid;
            in["gas_kohm"] = serialized(String(er.gas_kohm, 1));
            in["air_ready"] = er.air_ready;
            in["air_score"] = serialized(String(er.air_score, 0));
            in["air_level"] = env_sensor::airLevelName(er.air_level);
            in["air_baseline_kohm"] = serialized(String(er.air_baseline_kohm, 1));
            in["trend_air"] = env_sensor::trendName(er.t_air);
            in["d_air"] = serialized(String(er.d_air, 0));
          }
          if (er.has_humidity) {
            in["dew_point_c"] = serialized(String(er.dew_point_c, 1));
            in["abs_humidity"] = serialized(String(er.abs_humidity, 1));
            in["heat_index_c"] = serialized(String(er.heat_index_c, 1));
            in["condensation"] = env_sensor::condensationName(er.condensation);
            in["mould_risk"] = env_sensor::mouldName(er.mould_risk);
          }
          {
            WeatherData w;
            int wd = (shared::getWeather(w) && w.valid) ? w.cur.wind_dir : -1;
            zambretti::Result z = zambretti::forecast(er.sea_level_known ? er.sea_level_hpa : er.pressure_hpa, er.d_press, wd);
            JsonObject zo = in["forecast"].to<JsonObject>();
            zo["text"] = er.span_min >= 30 ? z.text : "";
            zo["z"] = er.span_min >= 30 ? z.z : 0;
            zo["trend"] = z.trend;
          }
        }
        in["errors"] = env_sensor::errors();
        in["errors_in_a_row"] = env_sensor::consecutiveErrors();
        in["last_error"] = env_sensor::lastError();
      }
      const i2c_bus::Map& m = i2c_bus::map();
      JsonObject i2c = root["i2c"].to<JsonObject>();
      i2c["es8311"] = m.es8311; i2c["pca9557"] = m.pca9557; i2c["rtc"] = m.rtc;
      {
        JsonArray found = i2c["found"].to<JsonArray>();
        for (uint8_t k = 0; k < m.n; k++) { char hex[6]; snprintf(hex, sizeof(hex), "0x%02X", m.found[k]); found.add(hex); }
      }
      res->setLength();
      r->send(res);
    }

    void handleConfigGet(AsyncWebServerRequest* r) {
      AsyncJsonResponse* res = new AsyncJsonResponse();
      JsonObject root = res->getRoot().to<JsonObject>();
      const bool download = r->hasParam("download");     // backup: unmasked secrets, served as a file
      app::cfgLock();
      config_to_json(g_cfg, root, !download);
      app::cfgUnlock();
      if (download) {
        res->addHeader("Content-Disposition", "attachment; filename=\"matrix-weather-clock-config.json\"");
        res->setLength();
        r->send(res);
        return;
      }
      JsonArray pl = root["pages_available"].to<JsonArray>();
      for (uint8_t i = 0; i < PAGE_COUNT; i++) {
        JsonObject o = pl.add<JsonObject>();
        o["id"] = page_name(i);
        o["label"] = page_label(i);
      }
      // The two theme tables, so the UI's tick boxes and pickers are generated from the firmware rather than
      // duplicated in JavaScript. "holiday" says which are chosen by the date; "bit" is the holidays_enabled bit.
      JsonArray th = root["themes_available"].to<JsonArray>();
      for (size_t i = 0; i < themes::HOLIDAY_COUNT; i++) {
        JsonObject o = th.add<JsonObject>();
        o["id"] = themes::HOLIDAYS[i].id;
        o["name"] = themes::HOLIDAYS[i].name;
        o["holiday"] = true;
        o["bit"] = (uint8_t)i;
      }
      for (size_t i = 0; i < themes::PALETTE_COUNT; i++) {
        JsonObject o = th.add<JsonObject>();
        o["id"] = themes::PALETTES[i].id;
        o["name"] = themes::PALETTES[i].name;
        o["holiday"] = false;
      }
      JsonArray tz = root["tz_options"].to<JsonArray>();     // short list only; the full table is /api/timezones
      for (size_t i = 0; i < TZ_COMMON_LEN && i < TZ_TABLE_LEN; i++) {
        JsonObject o = tz.add<JsonObject>();
        o["id"] = TZ_TABLE[i].id; o["label"] = TZ_TABLE[i].label; o["posix"] = TZ_TABLE[i].posix;
      }
      res->setLength();
      r->send(res);
    }

    // The whole zone table, fetched once by the settings page. Deliberately not part of /api/config, and
    // deliberately not built as a JsonDocument either: ArduinoJson copies every string, so the 5 KB of text cost
    // about 40 KB of internal heap and did not hand it straight back. The list is generated a chunk at a time
    // instead, out of flash, with one small buffer on the stack and nothing retained.
    size_t tzPiece(size_t i, char* out, size_t n) {
      const int w = (i >= TZ_TABLE_LEN)
        ? snprintf(out, n, "]")
        : snprintf(out, n, "%s{\"id\":\"%s\",\"label\":\"%s\",\"posix\":\"%s\"}",
                   i ? "," : "[", TZ_TABLE[i].id, TZ_TABLE[i].label, TZ_TABLE[i].posix);
      // snprintf reports the length it wanted, which can exceed the buffer. The longest entry in the table today is
      // 88 bytes against a 176-byte buffer, but clamping means a longer one added later truncates the JSON rather
      // than walking the caller off the end of it.
      if (w < 0) return 0;
      return (size_t)w >= n ? n - 1 : (size_t)w;
    }
    void handleTimezones(AsyncWebServerRequest* r) {
      AsyncWebServerResponse* res = r->beginChunkedResponse("application/json",
        [](uint8_t* buf, size_t maxLen, size_t index) -> size_t {
          size_t written = 0, pos = 0;
          for (size_t i = 0; i <= TZ_TABLE_LEN && written < maxLen; i++) {
            char piece[176];
            const size_t len = tzPiece(i, piece, sizeof(piece));
            const size_t want = index + written;          // absolute offset of the next byte owed
            if (pos + len > want) {
              const size_t off = want - pos;
              size_t take = len - off;
              if (take > maxLen - written) take = maxLen - written;
              memcpy(buf + written, piece + off, take);
              written += take;
            }
            pos += len;
          }
          return written;                                  // 0 ends the response
        });
      res->addHeader("Cache-Control", "max-age=86400");     // the table only changes with the firmware
      r->send(res);
    }

    void handleConfigPost(AsyncWebServerRequest* r, JsonVariant& json) {
      app::cfgLock();
      AppConfig next = g_cfg;
      app::cfgUnlock();
      uint32_t changed = 0;
      String err;
      if (!config_from_json(json.as<JsonObjectConst>(), next, changed, err)) { sendJsonError(r, 400, err); return; }
      // timezone id from the table fills tz_posix unless a custom string was sent
      JsonObjectConst tj = json.as<JsonObjectConst>()["time"];
      if (!tj.isNull() && tj["tz_id"].is<const char*>() && !tj["tz_posix"].is<const char*>()) {
        const char* posix = tz_posix_for(tj["tz_id"].as<const char*>());
        if (posix) strlcpy(next.time.tz_posix, posix, sizeof(next.time.tz_posix));
      }
      // display.color_preset is write-only: naming a palette copies its six colours into display.colors and is
      // never stored, so the colour pickers are always the truth and cannot drift from a remembered preset.
      JsonObjectConst dj = json.as<JsonObjectConst>()["display"];
      if (!dj.isNull() && dj["color_preset"].is<const char*>()) {
        const char* pid = dj["color_preset"].as<const char*>();
        if (*pid) {
          const themes::Theme* pt = themes::byId(pid);
          if (!pt) { sendJsonError(r, 400, "display.color_preset: unknown theme id"); return; }
          ColorsConfig& cc = next.display.colors;
          cc.time = pt->time; cc.date = pt->date; cc.temp = pt->temp;
          cc.text = pt->text; cc.hi = pt->hi;     cc.lo = pt->lo;
          changed |= CHG_DISPLAY;
        }
      }

      // same courtesy for the world clock: a zone id alone is enough, the table fills in the rule and the label
      JsonVariantConst wj = json.as<JsonObjectConst>()["world"];
      if (!wj.isNull()) {
        JsonArrayConst zs = wj["zones"];
        if (!zs.isNull()) {
          uint8_t i = 0;
          for (JsonObjectConst e : zs) {
            if (i >= MAX_WORLD_ZONES) break;
            WorldZone& z = next.world.zones[i];
            if (e["id"].is<const char*>() && !e["posix"].is<const char*>()) {
              const char* pz = tz_posix_for(e["id"].as<const char*>());
              strlcpy(z.posix, pz ? pz : "", sizeof(z.posix));
            }
            if (z.label[0] == '\0' && z.id[0]) {
              const char* lz = tz_label_for(z.id);
              if (lz) strlcpy(z.label, lz, sizeof(z.label));
            }
            i++;
          }
        }
      }
      if ((changed & CHG_ALERTS) && next.alerts.enabled && next.alerts.user_agent_contact[0] == '\0') { sendJsonError(r, 400, "alerts.user_agent_contact: NWS requires a contact (e-mail or URL)"); return; }
      app::stageConfig(next, changed);
      AsyncJsonResponse* res = new AsyncJsonResponse();
      JsonObject root = res->getRoot().to<JsonObject>();
      root["ok"] = true;
      JsonArray applied = root["applied"].to<JsonArray>();
      JsonArray reboot = root["reboot_required"].to<JsonArray>();
      struct { uint32_t bit; const char* name; bool reboot; } sections[] = {
        { CHG_WIFI, "wifi", false }, { CHG_LOCATION, "location", false }, { CHG_TIME, "time", false }, { CHG_WEATHER, "weather", false },
        { CHG_ALERTS, "alerts", false }, { CHG_DISPLAY, "display", false }, { CHG_PANEL, "panel", true }, { CHG_AUDIO, "audio", false }, { CHG_INDOOR, "indoor", false }, { CHG_RADAR, "radar", false }, { CHG_REMOTE, "remote", false }, { CHG_TIDE, "tide", false },
        { CHG_ALARMS, "alarms", false }, { CHG_LIGHTNING, "lightning", false }, { CHG_PUSHBULLET, "pushbullet", false }, { CHG_UPDATE, "update", false },
        { CHG_WEBHOOK, "webhook", false }, { CHG_EVENT, "events", false }, { CHG_WORLD, "world", false } };
      for (auto& s : sections) if (changed & s.bit) (s.reboot ? reboot : applied).add(s.name);
      res->setLength();
      r->send(res);
    }

    void handleWeather(AsyncWebServerRequest* r) {
      AsyncJsonResponse* res = new AsyncJsonResponse();
      WeatherData w;
      shared::getWeather(w);
      weatherToJson(res->getRoot().to<JsonObject>(), w);
      res->setLength();
      r->send(res);
    }

    void handleAlerts(AsyncWebServerRequest* r) {
      AsyncJsonResponse* res = new AsyncJsonResponse();
      JsonObject root = res->getRoot().to<JsonObject>();
      app::cfgLock();
      AlertsConfig ac = g_cfg.alerts;
      app::cfgUnlock();
      root["stale"] = alerts::stale();
      alerts::toJson(root["items"].to<JsonArray>(), ac, r->hasParam("all"));
      res->setLength();
      r->send(res);
    }

    void handleTestAlert(AsyncWebServerRequest* r, JsonVariant& json) {
      JsonObjectConst o = json.as<JsonObjectConst>();
      Severity s = Severity::Severe;
      if (o["severity"].is<const char*>() && !severity_parse(o["severity"], s)) { sendJsonError(r, 400, "unknown severity"); return; }
      uint16_t minutes = o["minutes"] | 3;
      alerts::injectTest(o["event"] | "Test Alert", s, o["headline"] | "This is a test alert from the matrix weather clock", minutes, millis());
      r->send(200, "application/json", "{\"ok\":true}");
    }

    void handleTestChime(AsyncWebServerRequest* r, JsonVariant& json) {
      JsonObjectConst o = json.as<JsonObjectConst>();
      app::cfgLock();
      ChimeStyle style = g_cfg.audio.chime;
      app::cfgUnlock();
      if (o["style"].is<const char*>() && !chime_parse(o["style"], style)) { sendJsonError(r, 400, "unknown chime style"); return; }
      bool force = o["force"] | true;
      if (audio_out::chime(style == ChimeStyle::None ? ChimeStyle::TwoTone : style, force)) r->send(200, "application/json", "{\"ok\":true}");
      else sendJsonError(r, 409, String("chime suppressed: ") + audio_out::lastSuppressReason());
    }

    void handleTestSay(AsyncWebServerRequest* r, JsonVariant& json) {
      JsonObjectConst o = json.as<JsonObjectConst>();
      const char* text = o["text"] | "";
      if (!*text) { sendJsonError(r, 400, "text required"); return; }
      if (!voice::info().installed) { sendJsonError(r, 409, "no voice pack installed"); return; }
      audio_out::ClipRef c;
      if (!voice::lookup(text, c)) { sendJsonError(r, 404, "phrase not in voice pack"); return; }
      if (voice::say(text, o["force"] | true)) r->send(200, "application/json", "{\"ok\":true}");
      else sendJsonError(r, 409, String("speech suppressed: ") + voice::lastError());
    }

    void handleMessage(AsyncWebServerRequest* r, JsonVariant& json) {
      JsonObjectConst o = json.as<JsonObjectConst>();
      const char* text = o["text"] | "";
      if (!*text) { sendJsonError(r, 400, "text required"); return; }
      if (strlen(text) > 200) { sendJsonError(r, 400, "text too long (max 200)"); return; }
      long sec = o["seconds"] | 30;
      if (sec < 0 || sec > 86400) { sendJsonError(r, 400, "seconds out of range"); return; }
      uint32_t rgb = 0xFFFFFF;
      const char* col = o["color"] | "";
      if (col[0] == '#' && strlen(col) == 7) rgb = strtoul(col + 1, nullptr, 16);
      app::cfgLock();
      ChimeStyle style = g_cfg.audio.chime;
      app::cfgUnlock();
      renderer::showMessage(text, (uint32_t)sec * 1000UL, rgb);
      if (o["chime"] | false) audio_out::chime(style, o["force"] | false);
      r->send(200, "application/json", "{\"ok\":true}");
    }

    void handleSleep(AsyncWebServerRequest* r, JsonVariant& json) {
      JsonObjectConst o = json.as<JsonObjectConst>();
      long m = o["minutes"] | -1L;
      if (m < 0) { sendJsonError(r, 400, "minutes required (0 cancels)"); return; }
      if (m > 720) { sendJsonError(r, 400, "minutes out of range (0..720)"); return; }
      renderer::startSleep((uint32_t)m);
      r->send(200, "application/json", "{\"ok\":true}");
    }

    void handleStopwatch(AsyncWebServerRequest* r, JsonVariant& json) {
      const char* a = json.as<JsonObjectConst>()["action"] | "toggle";
      if (!strcmp(a, "start")) alarmclock::stopwatchStart();
      else if (!strcmp(a, "stop")) alarmclock::stopwatchStop();
      else if (!strcmp(a, "reset")) alarmclock::stopwatchReset();
      else if (!strcmp(a, "toggle")) alarmclock::stopwatchToggle();
      else { sendJsonError(r, 400, "action: start, stop, reset or toggle"); return; }
      AsyncJsonResponse* res = new AsyncJsonResponse();
      JsonObject root = res->getRoot().to<JsonObject>();
      root["ok"] = true;
      root["running"] = alarmclock::stopwatchRunning();
      root["ms"] = alarmclock::stopwatchMs();
      res->setLength();
      r->send(res);
    }

    void handleTestWebhook(AsyncWebServerRequest* r) {
      app::cfgLock();
      const bool configured = g_cfg.webhook.enabled && g_cfg.webhook.url[0];
      app::cfgUnlock();
      if (!configured) { sendJsonError(r, 409, "webhook not configured"); return; }
      webhook::notify(webhook::Event::Test, "Matrix Weather Clock", "Test notification from your clock");
      r->send(200, "application/json", "{\"ok\":true}");
    }

    void handleSayTime(AsyncWebServerRequest* r) {
      struct tm lt;
      if (!timesvc::localNow(lt)) { sendJsonError(r, 409, "clock not set yet"); return; }
      if (!voice::sayTime(ChimeStyle::None, lt.tm_hour, lt.tm_min, r->hasParam("force"))) {
        sendJsonError(r, 409, voice::lastError());
        return;
      }
      r->send(200, "application/json", "{\"ok\":true}");
    }

    void handleTimer(AsyncWebServerRequest* r, JsonVariant& json) {
      long sec = json.as<JsonObjectConst>()["seconds"] | 0;
      if (sec <= 0) { long m = json.as<JsonObjectConst>()["minutes"] | 0; sec = m * 60; }
      if (!alarmclock::startTimer((uint32_t)max(0L, sec))) { sendJsonError(r, 400, "seconds must be 1..86400"); return; }
      r->send(200, "application/json", "{\"ok\":true}");
    }

    void handleFrame(AsyncWebServerRequest* r) {
      app::trace("web", "frame");
      AsyncWebServerResponse* res = r->beginChunkedResponse("application/octet-stream",
        [](uint8_t* buf, size_t maxLen, size_t index) -> size_t { return frame_snapshot::copy(buf, index, maxLen); });
      res->addHeader("Cache-Control", "no-store");
      char dims[24];
      snprintf(dims, sizeof(dims), "%ux%u", frame_snapshot::width(), frame_snapshot::height());
      res->addHeader("X-Frame-Size", dims);
      r->send(res);
    }

    void handleWifiScan(AsyncWebServerRequest* r) {
      AsyncJsonResponse* res = new AsyncJsonResponse();
      JsonObject root = res->getRoot().to<JsonObject>();
      if (r->hasParam("start")) { wifi_mgr::startScan(); root["scanning"] = true; }
      else {
        int n = wifi_mgr::scanToJson(root["networks"].to<JsonArray>());
        root["scanning"] = n < 0;
        if (n == 0 && !r->hasParam("poll")) wifi_mgr::startScan();
      }
      res->setLength();
      r->send(res);
    }
  }

  void registerApi(AsyncWebServer& server) {
    server.on("/api/status", HTTP_GET, handleStatus);
    server.on("/api/config", HTTP_GET, handleConfigGet);
    server.on("/api/timezones", HTTP_GET, handleTimezones);
    server.on("/api/weather", HTTP_GET, handleWeather);
    server.on("/api/alerts", HTTP_GET, handleAlerts);
    server.on("/api/wifi/scan", HTTP_GET, handleWifiScan);
    server.on("/api/log", HTTP_GET, [](AsyncWebServerRequest* r) { r->send(200, "text/plain", log_dump()); });
    server.on("/api/test/panel", HTTP_POST, [](AsyncWebServerRequest* r) {
      long sec = 10;
      if (r->hasParam("sec", true)) sec = r->getParam("sec", true)->value().toInt();
      else if (r->hasParam("sec")) sec = r->getParam("sec")->value().toInt();
      renderer::requestTest((uint32_t)constrain(sec, 3, 300) * 1000UL);
      r->send(200, "application/json", "{\"ok\":true}");
    });
    server.on("/api/alerts/ack", HTTP_POST, [](AsyncWebServerRequest* r) {
      String id = r->hasParam("id", true) ? r->getParam("id", true)->value() : (r->hasParam("id") ? r->getParam("id")->value() : "all");
      alerts::acknowledge(id.c_str());
      r->send(200, "application/json", "{\"ok\":true}");
    });
    server.on("/api/frame", HTTP_GET, handleFrame);
    server.on("/api/demo", HTTP_POST, [](AsyncWebServerRequest* r) {
      app::trace("web", "demo");
      auto param = [&](const char* n, const char* def) -> String {
        if (r->hasParam(n, true)) return r->getParam(n, true)->value();
        if (r->hasParam(n)) return r->getParam(n)->value();
        return def;
      };
      bool on = param("on", "1") != "0" && param("on", "1") != "false";
      long minutes = param("minutes", "10").toInt();
      bool sound = param("sound", "0") == "1" || param("sound", "0") == "true";
      long start = param("start", "0").toInt();                       // scenario index to begin with (0 = first)
      renderer::setDemo(on, (uint32_t)constrain(minutes, 1L, 720L) * 60000UL, sound, (uint8_t)constrain(start, 0L, 63L));
      r->send(200, "application/json", on ? "{\"ok\":true,\"demo\":true}" : "{\"ok\":true,\"demo\":false}");
    });
    server.on("/api/update/check", HTTP_POST, [](AsyncWebServerRequest* r) { updater::requestCheck(); r->send(200, "application/json", "{\"ok\":true}"); });
    server.on("/api/update/install", HTTP_POST, [](AsyncWebServerRequest* r) { updater::requestInstall(); r->send(200, "application/json", "{\"ok\":true}"); });
    server.on("/api/voice/download", HTTP_POST, [](AsyncWebServerRequest* r) { voice_pack::requestDownload(); r->send(200, "application/json", "{\"ok\":true}"); });
    server.on("/api/radar/frame", HTTP_GET, [](AsyncWebServerRequest* r) {
      app::trace("web", "radar/frame");
      long i = r->hasParam("i") ? r->getParam("i")->value().toInt() : -1;
      uint8_t n = radar::frameCount();
      if (!n) { sendJsonError(r, 404, "no radar frames yet"); return; }
      uint8_t idx = (i < 0 || i >= n) ? (uint8_t)(n - 1) : (uint8_t)i;
      AsyncWebServerResponse* res = r->beginChunkedResponse("application/octet-stream",
        [idx](uint8_t* buf, size_t maxLen, size_t index) -> size_t { return radar::copyFrameBytes(idx, buf, index, maxLen); });
      char dims[16];
      snprintf(dims, sizeof(dims), "%ux%u", radar::W, radar::H);
      res->addHeader("X-Frame-Size", dims);
      char age[16];
      snprintf(age, sizeof(age), "%ld", (long)radar::frameAgeMin(idx));
      res->addHeader("X-Frame-Age-Min", age);
      res->addHeader("Cache-Control", "no-store");
      r->send(res);
    });
    server.on("/api/action", HTTP_POST, [](AsyncWebServerRequest* r) {
      String n = r->hasParam("name", true) ? r->getParam("name", true)->value() : (r->hasParam("name") ? r->getParam("name")->value() : "");
      actions::Id id;
      if (!actions::parse(n.c_str(), id) || id == actions::Id::None) { sendJsonError(r, 400, "unknown action"); return; }
      actions::run(id, "web");
      r->send(200, "application/json", "{\"ok\":true}");
    });
    server.on("/api/actions", HTTP_GET, [](AsyncWebServerRequest* r) {
      auto* res = new AsyncJsonResponse(false);
      JsonArray arr = res->getRoot()["actions"].to<JsonArray>();
      for (uint8_t i = 1; i < (uint8_t)actions::Id::COUNT; i++) {
        JsonObject a = arr.add<JsonObject>();
        a["name"] = actions::name((actions::Id)i);
        a["label"] = actions::label((actions::Id)i);
      }
      res->setLength();
      r->send(res);
    });
    server.on("/api/remote", HTTP_GET, [](AsyncWebServerRequest* r) {
      auto* res = new AsyncJsonResponse(false);
      JsonObject root = res->getRoot();
      ir_remote::Status rs = ir_remote::status();
      root["enabled"] = rs.enabled; root["pin"] = rs.pin; root["received"] = rs.received; root["learning"] = rs.learning;
      char hex[12]; snprintf(hex, sizeof(hex), "0x%08lX", (unsigned long)rs.last_code);
      root["last_code"] = rs.last_code ? hex : "";
      root["last_proto"] = rs.last_proto;
      root["last_ms"] = rs.last_ms;
      root["last_age_s"] = rs.last_ms ? (long)((millis() - rs.last_ms) / 1000) : -1L;
      res->setLength();
      r->send(res);
    });
    server.on("/api/remote/learn", HTTP_POST, [](AsyncWebServerRequest* r) { ir_remote::startLearn(); r->send(200, "application/json", "{\"ok\":true}"); });
    server.on("/api/radar/base", HTTP_GET, [](AsyncWebServerRequest* r) {
      uint8_t probe;
      if (!radar::copyBaseBytes(&probe, 0, 1)) { sendJsonError(r, 404, "no base map loaded"); return; }
      AsyncWebServerResponse* res = r->beginChunkedResponse("application/octet-stream",
        [](uint8_t* buf, size_t maxLen, size_t index) -> size_t { return radar::copyBaseBytes(buf, index, maxLen); });
      char dims[16];
      snprintf(dims, sizeof(dims), "%ux%u", radar::W, radar::H);
      res->addHeader("X-Frame-Size", dims);
      res->addHeader("Cache-Control", "no-store");
      r->send(res);
    });
    server.on("/api/radar", HTTP_GET, [](AsyncWebServerRequest* r) {
      auto* res = new AsyncJsonResponse(false);
      JsonObject root = res->getRoot();
      radar::Status rs = radar::status();
      root["enabled"] = rs.enabled; root["frames"] = rs.frames; root["error"] = rs.err; root["echo_near"] = rs.echo_near; root["echo_pct"] = rs.echo_pct;
      root["base"] = rs.base_state == 2 ? "ready" : rs.base_state == 1 ? "loading" : rs.base_state == 3 ? "error" : "off";
      root["last_ok_age_s"] = rs.last_ok_ms ? (long)((millis() - rs.last_ok_ms) / 1000) : -1L;
      JsonArray ages = root["age_min"].to<JsonArray>();
      for (uint8_t i = 0; i < rs.frames; i++) ages.add(radar::frameAgeMin(i));
      res->setLength();
      r->send(res);
    });
    server.on("/api/tide/refresh", HTTP_POST, [](AsyncWebServerRequest* r) { tide::requestRefresh(); r->send(200, "application/json", "{\"ok\":true}"); });
    server.on("/api/radar/refresh", HTTP_POST, [](AsyncWebServerRequest* r) { radar::requestRefresh(); r->send(200, "application/json", "{\"ok\":true}"); });
    server.on("/api/indoor/rescan", HTTP_POST, [](AsyncWebServerRequest* r) {
      // the scan itself runs on the main loop (the I2C driver misreports acknowledges when driven from this task)
      i2c_bus::requestRescan();
      env_sensor::requestRescan();
      r->send(200, "application/json", "{\"ok\":true,\"pending\":true}");
    });
    server.on("/api/indoor/history", HTTP_GET, [](AsyncWebServerRequest* r) {
      long minutes = r->hasParam("minutes") ? r->getParam("minutes")->value().toInt() : 180;
      long step = r->hasParam("step") ? r->getParam("step")->value().toInt() : 0;
      minutes = constrain(minutes, 10L, 1440L);
      if (step <= 0) step = minutes <= 180 ? 1 : (minutes <= 720 ? 5 : 10);
      step = constrain(step, 1L, 60L);
      static env_sensor::HistoryPoint* pts = nullptr;   // 23 KB: lives in PSRAM, allocated on first use
      if (!pts) pts = (env_sensor::HistoryPoint*)heap_caps_malloc(1440 * sizeof(env_sensor::HistoryPoint), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (!pts) { sendJsonError(r, 500, "no memory"); return; }
      size_t n = env_sensor::history(pts, minutes / step + 1, (uint16_t)minutes, (uint16_t)step);
      auto* res = new AsyncJsonResponse(false);
      JsonObject root = res->getRoot();
      root["sensor"] = env_sensor::typeName();
      root["step_min"] = step;
      root["has_gas"] = env_sensor::hasGas();
      JsonArray age = root["age_min"].to<JsonArray>(), t = root["temp_c"].to<JsonArray>(), h = root["humidity"].to<JsonArray>(), pr = root["pressure_hpa"].to<JsonArray>();
      JsonArray gk = root["gas_kohm"].to<JsonArray>(), air = root["air_score"].to<JsonArray>();
      for (size_t i = 0; i < n; i++) {
        age.add(pts[i].age_min);
        t.add(serialized(String(pts[i].temp_c, 2)));
        h.add(serialized(String(pts[i].humidity, 1)));
        pr.add(serialized(String(pts[i].pressure_hpa, 2)));
        gk.add(serialized(String(pts[i].gas_kohm, 1)));
        air.add(serialized(String(pts[i].air_score, 0)));
      }
      res->setLength();
      r->send(res);
    });
    server.on("/api/voice/phrases", HTTP_GET, [](AsyncWebServerRequest* r) {
      auto* res = new AsyncJsonResponse(false);
      JsonObject root = res->getRoot();
      voice::PackInfo pi = voice::info();
      root["installed"] = pi.installed;
      root["voice"] = pi.voice;
      root["version"] = pi.version;
      voice::phrases(root["phrases"].to<JsonArray>());
      res->setLength();
      r->send(res);
    });
    server.on("/api/show", HTTP_POST, [](AsyncWebServerRequest* r) {
      app::trace("web", "show");
      String which = r->hasParam("screen", true) ? r->getParam("screen", true)->value() : (r->hasParam("screen") ? r->getParam("screen")->value() : "forecast");
      if (renderer::requestFullScreen(which.c_str())) r->send(200, "application/json", "{\"ok\":true}");
      else sendJsonError(r, 409, "screen not available (not set up, or no data yet)");
    });
    server.on("/api/test/push", HTTP_POST, [](AsyncWebServerRequest* r) {
      if (pushbullet::notify("Matrix Weather Clock", "Test notification from your clock")) r->send(200, "application/json", "{\"ok\":true}");
      else sendJsonError(r, 409, "Pushbullet token not set");
    });
    server.on("/api/message/clear", HTTP_POST, [](AsyncWebServerRequest* r) { renderer::clearMessage(); r->send(200, "application/json", "{\"ok\":true}"); });
    server.on("/api/alarm/stop", HTTP_POST, [](AsyncWebServerRequest* r) { alarmclock::stop(); r->send(200, "application/json", "{\"ok\":true}"); });
    server.on("/api/alarm/snooze", HTTP_POST, [](AsyncWebServerRequest* r) { alarmclock::snooze(); r->send(200, "application/json", "{\"ok\":true}"); });
    server.on("/api/timer/cancel", HTTP_POST, [](AsyncWebServerRequest* r) { alarmclock::cancelTimer(); r->send(200, "application/json", "{\"ok\":true}"); });
    server.on("/api/refresh", HTTP_POST, [](AsyncWebServerRequest* r) { net_task::kick(net_task::JOB_WEATHER | net_task::JOB_ALERTS); r->send(200, "application/json", "{\"ok\":true}"); });
    server.on("/api/reboot", HTTP_POST, [](AsyncWebServerRequest* r) { r->send(200, "application/json", "{\"ok\":true}"); app::requestReboot(500); });
    server.on("/api/factory-reset", HTTP_POST, [](AsyncWebServerRequest* r) {
      bool confirmed = (r->hasParam("confirm", true) && r->getParam("confirm", true)->value() == "yes") || (r->hasParam("confirm") && r->getParam("confirm")->value() == "yes");
      if (!confirmed) { sendJsonError(r, 400, "send confirm=yes"); return; }
      r->send(200, "application/json", "{\"ok\":true}");
      app::requestFactoryReset();
    });

    auto* cfgPost = new AsyncCallbackJsonWebHandler("/api/config", handleConfigPost);
    cfgPost->setMethod(HTTP_POST);
    cfgPost->setMaxContentLength(8192);
    server.addHandler(cfgPost);
    auto* testAlert = new AsyncCallbackJsonWebHandler("/api/test/alert", handleTestAlert);
    testAlert->setMethod(HTTP_POST);
    server.addHandler(testAlert);
    auto* testChime = new AsyncCallbackJsonWebHandler("/api/test/chime", handleTestChime);
    testChime->setMethod(HTTP_POST);
    server.addHandler(testChime);
    auto* testSay = new AsyncCallbackJsonWebHandler("/api/test/say", handleTestSay);
    testSay->setMethod(HTTP_POST);
    server.addHandler(testSay);
    auto* message = new AsyncCallbackJsonWebHandler("/api/message", handleMessage);
    message->setMethod(HTTP_POST);
    server.addHandler(message);
    auto* timer = new AsyncCallbackJsonWebHandler("/api/timer", handleTimer);
    timer->setMethod(HTTP_POST);
    server.addHandler(timer);
    auto* sleep = new AsyncCallbackJsonWebHandler("/api/sleep", handleSleep);
    sleep->setMethod(HTTP_POST);
    server.addHandler(sleep);
    auto* stopwatch = new AsyncCallbackJsonWebHandler("/api/stopwatch", handleStopwatch);
    stopwatch->setMethod(HTTP_POST);
    server.addHandler(stopwatch);
    server.on("/api/test/webhook", HTTP_POST, handleTestWebhook);
    server.on("/api/test/say-time", HTTP_POST, handleSayTime);
  }
}
