#include "webhook.h"
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "http_util.h"
#include "net_task.h"
#include "wifi_manager.h"
#include "util/log.h"
#include "version.h"

namespace webhook {
  namespace {
    struct Item { char title[64]; char body[192]; Event ev; };
    constexpr uint8_t QUEUE_LEN = 4;
    Item queue[QUEUE_LEN];
    uint8_t qhead = 0, qcount = 0;
    SemaphoreHandle_t mtx = nullptr;
    Status st = {};

    bool take() { return mtx && xSemaphoreTake(mtx, pdMS_TO_TICKS(100)) == pdTRUE; }
    void give() { xSemaphoreGive(mtx); }

    const char* eventName(Event e) {
      switch (e) {
        case Event::Alert: return "alert";
        case Event::Lightning: return "lightning";
        case Event::Alarm: return "alarm";
        case Event::Air: return "air";
        case Event::Boot: return "boot";
        default: return "test";
      }
    }

    void send(const AppConfig& cfg, const Item& it) {
      JsonDocument doc;
      doc[cfg.webhook.title_key[0] ? cfg.webhook.title_key : "title"] = it.title;
      doc[cfg.webhook.body_key[0] ? cfg.webhook.body_key : "message"] = it.body;
      doc["event"] = eventName(it.ev);
      doc["device"] = cfg.wifi.hostname;
      String body;
      serializeJson(doc, body);
      http_util::Options opt;
      opt.userAgent = MWC_USER_AGENT_NAME "/" MWC_VERSION;
      opt.accept = "application/json";
      opt.timeoutMs = 12000;
      if (cfg.webhook.header_name[0]) {
        opt.headerName = cfg.webhook.header_name;
        opt.headerValue = cfg.webhook.header_value;
      }
      String err;
      int code = 0;
      // Many of these services answer 200 with no body and some answer 204; http_util only accepts 200, so a 204
      // arrives here as an error string. That is reported, not retried: a notification is not worth a second try.
      const bool ok = http_util::postJson(cfg.webhook.url, opt, body, [](Stream&, int) { return true; }, err, &code);
      if (ok) {
        st.sent++;
        st.last_err[0] = '\0';
        LOGI("webhook: sent %s", eventName(it.ev));
      } else {
        st.errors++;
        strlcpy(st.last_err, err.c_str(), sizeof(st.last_err));
        LOGW("webhook: %s", err.c_str());
      }
      st.last_ms = millis();
    }
  }

  void begin() { mtx = xSemaphoreCreateMutex(); }

  bool wants(Event ev, Severity sev) {
    const WebhookConfig& w = g_cfg.webhook;
    if (!w.enabled || !w.url[0]) return false;
    switch (ev) {
      case Event::Alert: return w.on_alerts && (uint8_t)sev >= (uint8_t)w.min_severity;
      case Event::Lightning: return w.on_lightning;
      case Event::Alarm: return w.on_alarms;
      case Event::Air: return w.on_air;
      case Event::Boot: return w.on_boot;
      default: return true;
    }
  }

  bool notify(Event ev, const char* title, const char* body) {
    if (!g_cfg.webhook.enabled || !g_cfg.webhook.url[0]) return false;
    if (!take()) return false;
    if (qcount < QUEUE_LEN) {
      Item& it = queue[(qhead + qcount) % QUEUE_LEN];
      strlcpy(it.title, title ? title : "Matrix Weather Clock", sizeof(it.title));
      strlcpy(it.body, body ? body : "", sizeof(it.body));
      it.ev = ev;
      qcount++;
    }
    give();
    net_task::kick(net_task::JOB_WEBHOOK);
    return true;
  }

  void runQueued(const AppConfig& cfg) {
    st.configured = cfg.webhook.enabled && cfg.webhook.url[0] != '\0';
    if (!st.configured) { if (take()) { qcount = 0; give(); } return; }
    if (!wifi_mgr::isConnected()) return;
    for (uint8_t n = 0; n < QUEUE_LEN; n++) {
      Item it;
      if (!take()) return;
      if (!qcount) { give(); return; }
      it = queue[qhead];
      qhead = (qhead + 1) % QUEUE_LEN;
      qcount--;
      give();
      send(cfg, it);
    }
  }

  Status status() { Status c = st; c.configured = g_cfg.webhook.enabled && g_cfg.webhook.url[0] != '\0'; return c; }
}
