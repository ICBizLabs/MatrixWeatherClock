#include "key_test.h"
#include <ArduinoJson.h>
#include "http_util.h"
#include "net_task.h"
#include "util/psram_alloc.h"
#include "util/log.h"
#include "version.h"

namespace key_test {
  namespace {
    portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    Result res = {};
    char pendingKey[100] = "";
    volatile bool pending = false;
    uint32_t startedMs = 0;

    void finish(bool ok, const char* msg) {
      portENTER_CRITICAL(&mux);
      res.running = false; res.done = true; res.ok = ok;
      strlcpy(res.msg, msg, sizeof(res.msg));
      portEXIT_CRITICAL(&mux);
      LOGI("key test %s: %s", res.service, msg);
    }

    // GET url with the key in header, then name() picks the account name out of the reply
    void check(const char* url, const char* header, const char* key, const JsonDocument& filter,
               const char* (*name)(JsonDocument&)) {
      http_util::Options opt;
      opt.userAgent = MWC_USER_AGENT_NAME "/" MWC_VERSION;
      opt.accept = "application/json";
      opt.timeoutMs = 15000;
      opt.headerName = header;
      opt.headerValue = key;
      JsonDocument doc(psramAllocator());
      String err;
      int code = 0;
      JsonDocument wrapped;                      // OpenXBL wraps its reply in {"content": ...}
      wrapped.set(filter);
      wrapped["content"] = filter;
      const bool ok = http_util::get(url, opt, [&](Stream& s, int) {
        return deserializeJson(doc, s, DeserializationOption::Filter(wrapped)) == DeserializationError::Ok;
      }, err, &code);
      if (ok && !doc["content"].isNull()) { JsonDocument inner(psramAllocator()); inner.set(doc["content"]); doc = std::move(inner); }
      char msg[64];
      if (ok) snprintf(msg, sizeof(msg), "key works: %s", name(doc)[0] ? name(doc) : "account found");
      else if (code == 401 || code == 403) snprintf(msg, sizeof(msg), "key refused (HTTP %d)", code);
      else if (code == 429) snprintf(msg, sizeof(msg), "rate limited, try again later");
      else snprintf(msg, sizeof(msg), "no answer: %s", err.c_str());
      finish(ok, msg);
    }

    const char* pushbulletName(JsonDocument& d) { return d["name"] | (d["email"] | ""); }

    const char* xboxName(JsonDocument& d) {
      JsonArrayConst settings = d["profileUsers"][0]["settings"];
      for (JsonVariantConst s : settings)
        if (!strcmp(s["id"] | "", "Gamertag")) return s["value"] | "";
      return "";
    }
  }

  bool start(const char* service, const char* key) {
    if (strcmp(service, "pushbullet") && strcmp(service, "xbox")) return false;
    portENTER_CRITICAL(&mux);
    const bool busy = res.running && millis() - startedMs < 30000;   // a test stuck without WiFi gives way
    if (!busy) {
      res = {};
      res.running = true;
      startedMs = millis();
      strlcpy(res.service, service, sizeof(res.service));
      strlcpy(pendingKey, key ? key : "", sizeof(pendingKey));
    }
    portEXIT_CRITICAL(&mux);
    if (busy) return false;
    pending = true;
    net_task::kick(0);               // wake the network task; the test runs before its other work
    return true;
  }

  void run(const AppConfig& cfg) {
    if (!pending) return;
    pending = false;
    char service[12], key[100];
    portENTER_CRITICAL(&mux);
    strlcpy(service, res.service, sizeof(service));
    strlcpy(key, pendingKey, sizeof(key));
    pendingKey[0] = '\0';
    portEXIT_CRITICAL(&mux);
    const bool pb = !strcmp(service, "pushbullet");
    if (!key[0] || !strcmp(key, "***")) strlcpy(key, pb ? cfg.pushbullet.token : cfg.xbox.api_key, sizeof(key));
    if (!key[0]) { finish(false, "no key entered or stored"); return; }
    JsonDocument filter;
    if (pb) {
      filter["name"] = true;
      filter["email"] = true;
      check("https://api.pushbullet.com/v2/users/me", "Access-Token", key, filter, pushbulletName);
    } else {
      filter["profileUsers"][0]["settings"][0]["id"] = true;
      filter["profileUsers"][0]["settings"][0]["value"] = true;
      check("https://xbl.io/api/v2/account", "X-Authorization", key, filter, xboxName);
    }
  }

  Result result() {
    portENTER_CRITICAL(&mux);
    Result c = res;
    portEXIT_CRITICAL(&mux);
    return c;
  }
}
