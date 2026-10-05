#include "app.h"
#include <esp_heap_caps.h>
#include <esp_attr.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "display/renderer.h"
#include "display/panel.h"
#include "audio/audio_out.h"
#include "audio/voice.h"
#include "io/env_sensor.h"
#include "net/radar.h"
#include "net/tide.h"
#include "io/ir_remote.h"
#include "net/wifi_manager.h"
#include "net/net_task.h"
#include "net/lightning.h"
#include "time/time_service.h"
#include "util/log.h"

namespace app {
  namespace {
    SemaphoreHandle_t cfgMtx = nullptr;
    AppConfig staged;
    uint32_t stagedFlags = 0;
    volatile bool hasStaged = false;
    volatile bool factoryReset = false;
    uint32_t rebootAt = 0;
    uint32_t rebootFlags = 0;
    uint32_t bootMs = 0;

    void applyChanges(uint32_t fl) {
      if (fl & CHG_TIME) timesvc::applyTz(g_cfg.time);
      if (fl & CHG_DISPLAY) { panel::setGamma(g_cfg.display.gamma); renderer::applyDisplay(); }
      if (fl & CHG_AUDIO) { audio_out::apply(g_cfg.audio); voice::apply(g_cfg.audio); }
      if (fl & (CHG_INDOOR | CHG_WEATHER)) env_sensor::apply(g_cfg.indoor);
      if (fl & (CHG_RADAR | CHG_LOCATION)) radar::applyConfig();
      if (fl & CHG_REMOTE) ir_remote::apply(g_cfg.remote);
      if (fl & (CHG_TIDE | CHG_WEATHER)) tide::applyConfig();
      if (fl & (CHG_WEATHER | CHG_LOCATION)) net_task::kick(net_task::JOB_WEATHER);
      if (fl & (CHG_ALERTS | CHG_LOCATION)) net_task::kick(net_task::JOB_ALERTS);
      if (fl & CHG_WIFI) wifi_mgr::applyCredentials(g_cfg.wifi);
      if (fl & CHG_PANEL) { rebootFlags |= CHG_PANEL; panel::setLatchBlanking(g_cfg.panel.latch_blanking); panel::setRotation(g_cfg.panel.rotation); }
      if (fl & (CHG_LIGHTNING | CHG_LOCATION)) lightning::applyConfig();
    }
  }

  namespace {
    struct BlackBox { uint32_t magic; char where[40]; uint32_t uptime_s; uint32_t heap; };
    constexpr uint32_t BB_MAGIC = 0x4D574342;   // "MWCB"
    LastReset prevReset;
  }
  RTC_NOINIT_ATTR static BlackBox g_bb;

  void begin() {
    cfgMtx = xSemaphoreCreateRecursiveMutex();
    bootMs = millis();
    prevReset.reason = (int)esp_reset_reason();
    const int r = prevReset.reason;
    const bool unexpected = r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT || r == ESP_RST_BROWNOUT;
    if (g_bb.magic == BB_MAGIC && unexpected) {
      prevReset.valid = true;
      memcpy(prevReset.where, g_bb.where, sizeof(prevReset.where));
      prevReset.where[sizeof(prevReset.where) - 1] = '\0';
      prevReset.uptime_s = g_bb.uptime_s;
      prevReset.heap = g_bb.heap;
    }
    g_bb.magic = BB_MAGIC;
    g_bb.where[0] = '\0';
    g_bb.uptime_s = 0;
    g_bb.heap = 0;
  }

  void trace(const char* what, const char* detail) {
    char* w = g_bb.where;
    size_t n = strlcpy(w, what ? what : "", sizeof(g_bb.where));
    if (detail && n + 1 < sizeof(g_bb.where)) { w[n++] = ':'; strlcpy(w + n, detail, sizeof(g_bb.where) - n); }
    g_bb.uptime_s = (millis() - bootMs) / 1000;
    g_bb.heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }

  LastReset lastReset() { return prevReset; }

  const char* resetReasonName(int r) {
    switch (r) {
      case ESP_RST_POWERON: return "power-on";
      case ESP_RST_EXT: return "external";
      case ESP_RST_SW: return "software";
      case ESP_RST_PANIC: return "panic";
      case ESP_RST_INT_WDT: return "interrupt watchdog";
      case ESP_RST_TASK_WDT: return "task watchdog";
      case ESP_RST_WDT: return "watchdog";
      case ESP_RST_DEEPSLEEP: return "deep sleep";
      case ESP_RST_BROWNOUT: return "brownout";
      case ESP_RST_SDIO: return "sdio";
      default: return "unknown";
    }
  }

  void cfgLock() { if (cfgMtx) xSemaphoreTakeRecursive(cfgMtx, pdMS_TO_TICKS(1000)); }
  void cfgUnlock() { if (cfgMtx) xSemaphoreGiveRecursive(cfgMtx); }

  bool stageConfig(const AppConfig& next, uint32_t changed) {
    cfgLock();
    staged = next;
    stagedFlags |= changed;
    hasStaged = true;
    cfgUnlock();
    return true;
  }

  void requestReboot(uint32_t delay_ms) { rebootAt = millis() + delay_ms; if (rebootAt == 0) rebootAt = 1; }
  void requestFactoryReset() { factoryReset = true; }
  bool rebootPending() { return rebootAt != 0; }
  uint32_t uptimeSec() { return (millis() - bootMs) / 1000; }
  uint32_t rebootRequiredFlags() { return rebootFlags; }

  void loop() {
    if (hasStaged) {
      cfgLock();
      AppConfig next = staged;
      uint32_t fl = stagedFlags;
      hasStaged = false;
      stagedFlags = 0;
      g_cfg = next;
      cfgUnlock();
      LOGI("config: applying changes 0x%06X", (unsigned)fl);
      applyChanges(fl);
      config_save(g_cfg);
    }
    if (g_cfg.first_boot && uptimeSec() > 15) {   // the first-boot test pattern has been shown; remember that
      g_cfg.first_boot = false;
      config_save(g_cfg);
    }
    if (factoryReset) { factoryReset = false; config_factory_reset(); }
    if (rebootAt && (int32_t)(millis() - rebootAt) >= 0) {
      LOGW("rebooting");
      delay(100);
      ESP.restart();
    }
  }
}
