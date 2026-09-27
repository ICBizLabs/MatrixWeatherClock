#include "net_task.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include "app.h"
#include "config/config.h"
#include "wifi_manager.h"
#include "weather_client.h"
#include "alerts_client.h"
#include "alert_store.h"
#include "shared_state.h"
#include "pushbullet.h"
#include "webhook.h"
#include "updater.h"
#include "voice_pack.h"
#include "radar.h"
#include "tide.h"
#include "util/log.h"

namespace net_task {
  namespace {
    QueueHandle_t q = nullptr;
    volatile bool paused = false;
    uint32_t nextWx = 0, nextAl = 0;
    uint8_t wxFails = 0, alFails = 0;

    uint32_t backoff(uint8_t fails, uint32_t base_ms, uint32_t max_ms = 15 * 60000UL) {
      uint32_t d = base_ms;
      for (uint8_t i = 0; i < fails && d < max_ms; i++) d *= 2;
      if (d > max_ms) d = max_ms;
      return d + (esp_random() % (d / 10 + 1));
    }

    struct WxUpd { bool ok; const char* err; uint32_t next; uint8_t fails; };
    void wxNet(NetStatus& n, void* a) {
      WxUpd* u = (WxUpd*)a;
      if (u->ok) { n.last_wx_ok = millis(); n.wx_err[0] = 0; } else { n.last_wx_err = millis(); strlcpy(n.wx_err, u->err, sizeof(n.wx_err)); }
      n.next_wx = u->next; n.wx_fails = u->fails;
    }
    void alNet(NetStatus& n, void* a) {
      WxUpd* u = (WxUpd*)a;
      if (u->ok) { n.last_al_ok = millis(); n.al_err[0] = 0; } else { n.last_al_err = millis(); strlcpy(n.al_err, u->err, sizeof(n.al_err)); }
      n.next_al = u->next; n.al_fails = u->fails;
    }

    void runWeather(const AppConfig& cfg) {
      WeatherData w;
      String err;
      bool ok = weather_client::fetch(cfg, w, err);
      if (ok) { shared::setWeather(w); wxFails = 0; nextWx = millis() + (uint32_t)cfg.weather.refresh_min * 60000UL; }
      else { if (wxFails < 10) wxFails++; nextWx = millis() + backoff(wxFails, 30000); LOGW("weather: %s (retry in %lus)", err.c_str(), (unsigned long)((nextWx - millis()) / 1000)); }
      WxUpd u = { ok, err.c_str(), nextWx, wxFails };
      shared::updateNet(wxNet, &u);
    }

    void runAlerts(const AppConfig& cfg) {
      String err;
      if (cfg.alerts.user_agent_contact[0] == '\0') {   // NWS rejects requests without a contact; wait until one is set
        err = "no NWS contact set (Location tab)";
        nextAl = millis() + 60000;
        WxUpd u = { false, err.c_str(), nextAl, alFails };
        shared::updateNet(alNet, &u);
        return;
      }
      bool ok = alerts_client::fetch(cfg, err);
      if (ok) { alFails = 0; nextAl = millis() + (uint32_t)cfg.alerts.poll_sec * 1000UL; }
      else {
        alerts::pollFailed();
        if (alFails < 10) alFails++;
        uint32_t d = backoff(alFails, 30000);
        if (err.startsWith("http 429") || err.startsWith("http 503")) d = max<uint32_t>(d, 10 * 60000UL);
        nextAl = millis() + d;
        LOGW("alerts: %s (retry in %lus)", err.c_str(), (unsigned long)(d / 1000));
      }
      WxUpd u = { ok, err.c_str(), nextAl, alFails };
      shared::updateNet(alNet, &u);
    }

    void task(void*) {
      uint8_t forced = 0;
      for (;;) {
        uint8_t job = 0;
        if (xQueueReceive(q, &job, pdMS_TO_TICKS(1000)) == pdTRUE) forced |= job;
        if (paused || !wifi_mgr::isConnected()) continue;
        AppConfig cfg;
        app::cfgLock(); cfg = g_cfg; app::cfgUnlock();
        uint32_t now = millis();
        if (cfg.weather.enabled && ((forced & JOB_WEATHER) || (int32_t)(now - nextWx) >= 0)) { forced &= ~JOB_WEATHER; app::trace("net", "weather"); runWeather(cfg); }
        if (paused) continue;
        now = millis();
        if (cfg.alerts.enabled && ((forced & JOB_ALERTS) || (int32_t)(now - nextAl) >= 0)) { forced &= ~JOB_ALERTS; app::trace("net", "alerts"); runAlerts(cfg); }
        if (paused) continue;
        if ((forced & JOB_RADAR) || radar::due(cfg, millis())) { forced &= ~JOB_RADAR; app::trace("net", "radar"); radar::run(cfg); }
        if (paused) continue;
        if ((forced & JOB_TIDE) || tide::due(cfg, millis())) { forced &= ~JOB_TIDE; app::trace("net", "tide"); tide::run(cfg); }
        if (paused) continue;
        forced &= ~JOB_WEBHOOK;
        app::trace("net", "webhook");
        webhook::runQueued(cfg);
        if (paused) continue;
        forced &= ~JOB_PUSH;
        app::trace("net", "pushbullet");
        pushbullet::runQueued(cfg);
        if (pushbullet::due(cfg, millis())) pushbullet::poll(cfg);
        if (paused) continue;
        forced &= ~JOB_UPDATE;
        app::trace("net", "updater");
        updater::run(cfg);
        if (paused) continue;
        app::trace("net", "voice");
        voice_pack::run(cfg);
        app::trace("net", "idle");
      }
    }
  }

  void start() {
    q = xQueueCreate(8, sizeof(uint8_t));
    nextWx = nextAl = millis();
    xTaskCreatePinnedToCore(task, "net", 16384, nullptr, 2, nullptr, 0);
  }

  void pause(bool p) { paused = p; }

  void kick(uint8_t jobs) { if (q) xQueueSend(q, &jobs, 0); }
}
