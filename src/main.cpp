// Matrix Weather Clock - Seengreat RGB Matrix HUB75 S3 + 64x32 HUB75 panel.
// Clock + local weather (Open-Meteo) + NWS alerts with a chime, configured through the built-in web UI.
#include <Arduino.h>
#include <LittleFS.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include "pins.h"
#include "version.h"
#include "app.h"
#include "util/log.h"
#include "io/i2c_bus.h"
#include "io/buttons.h"
#include "io/env_sensor.h"
#include "io/ir_remote.h"
#include "config/config.h"
#include "display/panel.h"
#include "display/canvas.h"
#include "display/layout.h"
#include "display/renderer.h"
#include "display/frame_snapshot.h"
#include "alarm/alarm.h"
#include "net/lightning.h"
#include "net/pushbullet.h"
#include "net/updater.h"
#include "net/voice_pack.h"
#include "net/radar.h"
#include "net/tide.h"
#include "util/moon.h"
#include "net/shared_state.h"
#include "net/alert_store.h"
#include "net/wifi_manager.h"
#include "net/net_task.h"
#include "time/time_service.h"
#include "audio/audio_out.h"
#include "audio/voice.h"
#include "web/web_server.h"

static Canvas canvas(layout::W, layout::H);
static uint32_t lastFrame = 0, lastSecond = 0;
static constexpr uint32_t FRAME_MS = 33;
static constexpr uint32_t BOOT_OK_AFTER_MS = 30000;
static constexpr uint32_t MAX_FAILED_BOOTS = 3;
RTC_NOINIT_ATTR static uint32_t g_bootAttempts;   // survives resets (not power cycles): catches boot loops caused by panel settings
static bool bootConfirmed = false;

// puts the sensor pages into the rotation when a sensor is present (in RAM; saving the Display tab keeps them)
static void addSensorPages() {
  if (!env_sensor::present() || !g_cfg.indoor.auto_page) return;
  auto addPage = [](uint8_t id) {
    for (uint8_t i = 0; i < g_cfg.display.npages; i++) if (g_cfg.display.pages[i] == id) return;
    if (g_cfg.display.npages < PAGE_COUNT) g_cfg.display.pages[g_cfg.display.npages++] = id;
  };
  addPage(PAGE_INDOOR);
  addPage(PAGE_BARO);
  if (env_sensor::hasGas()) addPage(PAGE_AIR);
}

static void onButton(uint8_t key, bool longPress) {
  LOGI("btn K%u %s", key + 1, longPress ? "long" : "short");
  if (alarmclock::ringing()) {                       // any key deals with a ringing alarm or timer first
    if (key == buttons::K2 || longPress) alarmclock::stop(); else alarmclock::snooze();
    return;
  }
  switch (key) {
    case buttons::K1: if (longPress) renderer::requestTest(10000); else renderer::nextPage(); break;
    case buttons::K2:
      if (longPress) audio_out::chime(g_cfg.audio.chime, true);
      else if (renderer::demoActive()) renderer::setDemo(false);
      else if (renderer::hasMessage()) renderer::clearMessage();
      else alerts::acknowledge("all");
      break;
    case buttons::K3: if (longPress) app::requestReboot(200); else net_task::kick(net_task::JOB_WEATHER | net_task::JOB_ALERTS); break;
  }
}

void setup() {
  // Send every allocation of 4 KB or more to PSRAM (default threshold is 16 KB). Keeps internal RAM free and
  // unfragmented for WiFi, TLS handshakes and task stacks; DMA buffers and stacks ask for internal RAM explicitly.
  heap_caps_malloc_extmem_enable(4096);
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 1500) delay(10);
  log_begin();
  LOGI("MWC %s boot, heap %lu, psram %lu", MWC_VERSION, (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getFreePsram());

  if (!LittleFS.begin(true)) LOGE("LittleFS mount failed");
  config_load();
  app::begin();
  {
    app::LastReset lr = app::lastReset();
    if (lr.valid) LOGW("previous run ended by %s after %lu s while %s (internal heap %lu)", app::resetReasonName(lr.reason), (unsigned long)lr.uptime_s, lr.where[0] ? lr.where : "?", (unsigned long)lr.heap);
    else LOGI("reset reason: %s", app::resetReasonName(lr.reason));
  }
  shared::begin();
  alerts::begin();
  pushbullet::begin();
  updater::begin();
  radar::begin();
  tide::begin();

  i2c_bus::begin();
  const i2c_bus::Map& i2c = i2c_bus::identify();

  esp_reset_reason_t rr = esp_reset_reason();
  if (rr == ESP_RST_POWERON || rr == ESP_RST_UNKNOWN) g_bootAttempts = 0;   // RTC memory holds garbage after power-on
  g_bootAttempts++;
  if (g_bootAttempts > MAX_FAILED_BOOTS) {
    LOGE("boot loop detected (%lu resets, reason %d): panel settings reset to defaults, brightness lowered",
         (unsigned long)g_bootAttempts, (int)rr);
    g_cfg.panel = PanelConfig();
    if (g_cfg.display.brightness > 64) g_cfg.display.brightness = 64;
    config_save(g_cfg);
    g_bootAttempts = 0;
  }

  if (!panel::begin(g_cfg.panel, g_cfg.display.brightness, g_cfg.display.gamma)) LOGE("HUB75 begin failed");
  else LOGI("HUB75 ok %ux%u @ %d Hz", g_cfg.panel.width, g_cfg.panel.height, panel::refreshRateHz());
  frame_snapshot::begin(layout::W, layout::H);
  renderer::begin(millis());
  if (g_cfg.first_boot) renderer::requestTest(10000);
  alarmclock::begin();

  timesvc::begin(g_cfg.time);
  wifi_mgr::begin(g_cfg.wifi);
  web::begin();
  net_task::start();
  audio_out::begin(g_cfg.audio, i2c.es8311);
  voice::begin(g_cfg.audio);
  voice_pack::begin();
  buttons::begin(onButton);
  env_sensor::begin(g_cfg.indoor);
  addSensorPages();
  if (g_cfg.display.moon_page) {   // the moon page needs nothing but the date
    bool has = false;
    for (uint8_t i = 0; i < g_cfg.display.npages; i++) if (g_cfg.display.pages[i] == PAGE_MOON) has = true;
    if (!has && g_cfg.display.npages < PAGE_COUNT) g_cfg.display.pages[g_cfg.display.npages++] = PAGE_MOON;
  }
  lightning::begin();
  ir_remote::begin(g_cfg.remote);
  LOGI("setup done, heap %lu", (unsigned long)ESP.getFreeHeap());
}

void loop() {
  uint32_t now = millis();
  wifi_mgr::loop();
  timesvc::loop();
  buttons::loop();
  i2c_bus::loop();
  env_sensor::loop(now);
  if (env_sensor::consumeDetectedEvent()) { app::cfgLock(); addSensorPages(); app::cfgUnlock(); }
  ir_remote::loop();
  app::loop();
  if (!bootConfirmed && now > BOOT_OK_AFTER_MS) { bootConfirmed = true; g_bootAttempts = 0; }
  if (wifi_mgr::consumeConnectedEvent()) {
    renderer::showIp((uint32_t)g_cfg.display.ip_on_connect_sec * 1000UL);
    timesvc::onWifiUp(g_cfg.time);
    net_task::kick(net_task::JOB_WEATHER | net_task::JOB_ALERTS);
  }
  { uint8_t style; const char* phrase; if (renderer::consumeDemoSound(style, phrase)) voice::announce(voice::Kind::Demo, (ChimeStyle)style, phrase, true); }
  if (now - lastSecond >= 1000) {
    lastSecond = now;
    alerts::expire(time(nullptr));
    {
      static bool tidePageAdded = false;
      tide::Data td;
      if (!tidePageAdded && g_cfg.tide.auto_page && tide::get(td)) {
        tidePageAdded = true;
        app::cfgLock();
        bool has = false;
        for (uint8_t i = 0; i < g_cfg.display.npages; i++) if (g_cfg.display.pages[i] == PAGE_TIDE) has = true;
        if (!has && g_cfg.display.npages < PAGE_COUNT) g_cfg.display.pages[g_cfg.display.npages++] = PAGE_TIDE;
        app::cfgUnlock();
      }
    }
    if (env_sensor::present()) {
      WeatherData w;
      if (shared::getWeather(w)) {
        if (w.elevation_m > -9000) env_sensor::setAltitudeHint(w.elevation_m);
        env_sensor::setOutdoorTempC(w.imperial ? (w.cur.temp - 32.0f) * 5.0f / 9.0f : w.cur.temp);
      }
      if (env_sensor::consumeAirAlert()) {
        LOGI("indoor: air quality poor");
        voice::announce(voice::Kind::Indoor, g_cfg.audio.chime, "air quality poor", false);
        if (g_cfg.pushbullet.notify_air && g_cfg.pushbullet.token[0]) {
          env_sensor::Reading er = env_sensor::reading();
          char body[96];
          snprintf(body, sizeof(body), "Indoor air quality %d%% (%.0f kOhm, humidity %.0f%%). Time to ventilate.", (int)lroundf(er.air_score), er.gas_kohm, er.humidity);
          pushbullet::notify("Air quality poor", body);
        }
      }
    }
    Severity fired;
    char firedEvent[48];
    if (alerts::takeNewForChime(g_cfg.alerts, g_cfg.audio.repeat_min, now, &fired, firedEvent, sizeof(firedEvent)))
      voice::announce(voice::Kind::Alert, fired == Severity::Extreme ? g_cfg.audio.chime_extreme : g_cfg.audio.chime, firedEvent, false);
    alarmclock::loop(now);
    if (lightning::consumeChimeEvent()) voice::announce(voice::Kind::Lightning, g_cfg.audio.chime, "lightning nearby", false);
    if (lightning::consumeNotifyEvent() && g_cfg.pushbullet.notify_lightning && g_cfg.pushbullet.token[0]) {
      lightning::Status ls = lightning::status();
      char body[96];
      snprintf(body, sizeof(body), "Strike %.1f km %s of home, %u in the last %u min", ls.latest_km, lightning::bearingName(ls.latest_bearing), ls.count, g_cfg.lightning.window_min);
      pushbullet::notify("Lightning nearby", body);
    }
    if (g_cfg.pushbullet.notify_alerts && g_cfg.pushbullet.token[0]) {
      char ev[48], hl[160];
      if (alerts::takeNewForNotify(g_cfg.alerts, g_cfg.pushbullet.notify_min_severity, ev, sizeof(ev), hl, sizeof(hl))) pushbullet::notify(ev, hl);
    }
  }
  if (now - lastFrame >= FRAME_MS) {
    lastFrame = now;
    renderer::tick(canvas, now);
    panel::present(canvas);
    frame_snapshot::update(canvas.getBuffer());
  } else {
    delay(1);
  }
}
