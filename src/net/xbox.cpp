#include "xbox.h"
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "http_util.h"
#include "net_task.h"
#include "wifi_manager.h"
#include "display/renderer.h"
#include "util/psram_alloc.h"
#include "util/timeutil.h"
#include "util/log.h"
#include "version.h"

namespace xbox {
  namespace {
    constexpr const char* API = "https://xbl.io/api/v2";
    constexpr uint32_t FIRST_MS = 30000UL, FAST_MS = 5 * 60000UL, SLOW_MS = 15 * 60000UL, RETRY_MS = 10 * 60000UL;
    constexpr uint32_t BANNER_MS = 30000UL, BANNER_RGB = 0x20E020;   // Xbox green
    SemaphoreHandle_t mtx = nullptr;
    Data data;
    Status st = {};
    char xuid[24] = "";
    uint32_t nextFast = 0, nextSlow = 0, hourStart = 0;
    bool polledOnce = false;                   // banners only after the first full poll, so a boot stays quiet
    char lastTitleId[16] = "";
    uint16_t lastTitleCount = 0xFFFF;
    volatile bool refreshReq = false, resetReq = false;

    bool take() { return mtx && xSemaphoreTake(mtx, pdMS_TO_TICKS(100)) == pdTRUE; }
    void give() { xSemaphoreGive(mtx); }

    void fail(const char* what) {
      if (take()) { st.last_err_ms = millis(); if (st.fails < 10) st.fails++; strlcpy(st.err, what, sizeof(st.err)); give(); }
      LOGW("xbox: %s", what);
    }

    String urlEncode(const char* s) {
      String o;
      for (; *s; s++) {
        const unsigned char c = (unsigned char)*s;
        if (isalnum(c) || c == '-' || c == '_' || c == '.') o += (char)c;
        else { char b[4]; snprintf(b, sizeof(b), "%%%02X", c); o += b; }
      }
      return o;
    }

    // "2026-10-01T12:34:56.1234567Z" (UTC) -> epoch seconds
    time_t parseIso(const char* s) {
      int Y, M, D, h, m, sec;
      if (!s || sscanf(s, "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &sec) != 6) return 0;
      return (time_t)(days_from_civil(Y, (unsigned)M, (unsigned)D) * 86400LL + h * 3600 + m * 60 + sec);
    }

    // GET a path under the API into doc, keeping only what filter names (filter may be null for small replies)
    bool call(const AppConfig& cfg, const String& path, JsonDocument& doc, const JsonDocument* filter, int& code) {
      http_util::Options opt;
      opt.userAgent = MWC_USER_AGENT_NAME "/" MWC_VERSION;
      opt.accept = "application/json";
      opt.timeoutMs = 15000;
      opt.headerName = "X-Authorization";
      opt.headerValue = cfg.xbox.api_key;
      const uint32_t now = millis();
      if (now - hourStart >= 3600000UL) { hourStart = now; st.requests_hour = 0; }
      st.requests_hour++;
      String err;
      code = 0;
      bool ok = http_util::get(String(API) + path, opt, [&](Stream& s, int) {
        DeserializationError e = filter ? deserializeJson(doc, s, DeserializationOption::Filter(*filter))
                                        : deserializeJson(doc, s);
        return e == DeserializationError::Ok;
      }, err, &code);
      if (!ok) {
        if (code == 429) err = "rate limited (150 requests an hour)";
        else if (code == 401 || code == 403) err = "API key refused";
        fail(err.c_str());
      }
      return ok;
    }

    bool lookup(const AppConfig& cfg) {
      JsonDocument filter;
      filter["people"][0]["xuid"] = true;
      filter["people"][0]["gamertag"] = true;
      JsonDocument doc(psramAllocator());
      int code;
      if (!call(cfg, String("/friends/search?gt=") + urlEncode(cfg.xbox.gamertag), doc, &filter, code)) return false;
      const char* x = doc["people"][0]["xuid"] | "";
      if (!x[0]) { fail("gamertag not found"); return false; }
      strlcpy(xuid, x, sizeof(xuid));
      if (take()) { strlcpy(st.xuid, xuid, sizeof(st.xuid)); strlcpy(data.gamertag, doc["people"][0]["gamertag"] | cfg.xbox.gamertag, sizeof(data.gamertag)); give(); }
      LOGI("xbox: %s is xuid %s", cfg.xbox.gamertag, xuid);
      return true;
    }

    // online state and the foreground title
    bool fetchPresence(const AppConfig& cfg, Data& d) {
      JsonDocument doc(psramAllocator());
      int code;
      if (!call(cfg, String("/") + xuid + "/presence", doc, nullptr, code)) return false;
      JsonVariantConst p = doc.as<JsonVariantConst>();
      if (p.is<JsonArrayConst>()) p = p[0];
      const char* state = p["state"] | "Offline";
      d.online = !strcasecmp(state, "Online");
      d.game[0] = '\0';
      if (d.online) {
        JsonArrayConst devices = p["devices"];
        for (JsonVariantConst dev : devices) {
          JsonArrayConst titles = dev["titles"];
          for (JsonVariantConst t : titles) {
            const char* name = t["name"] | "";
            if (!strcmp(t["placement"] | "", "Full") && name[0] && strcasecmp(name, "Home")) strlcpy(d.game, name, sizeof(d.game));
          }
        }
      }
      return true;
    }

    // friends online; a private list leaves the count unknown rather than failing the poll
    void fetchFriends(const AppConfig& cfg, Data& d) {
      d.friends_online = -1;
      for (auto& n : d.friend_names) n[0] = '\0';
      if (!cfg.xbox.friends) return;
      JsonDocument filter;
      filter["people"][0]["gamertag"] = true;
      filter["people"][0]["presenceState"] = true;
      JsonDocument doc(psramAllocator());
      int code;
      if (!call(cfg, String("/friends?xuid=") + xuid, doc, &filter, code)) return;
      JsonArrayConst people = doc["people"];
      if (people.isNull()) return;
      int16_t n = 0;
      for (JsonVariantConst f : people) {
        if (strcasecmp(f["presenceState"] | "", "Online")) continue;
        if (n < 3) strlcpy(d.friend_names[n], f["gamertag"] | "", sizeof(d.friend_names[n]));
        n++;
      }
      d.friends_online = n;
    }

    bool fetchAccount(const AppConfig& cfg, Data& d) {
      JsonDocument filter;
      filter["profileUsers"][0]["settings"][0]["id"] = true;
      filter["profileUsers"][0]["settings"][0]["value"] = true;
      JsonDocument doc(psramAllocator());
      int code;
      if (!call(cfg, String("/account/") + xuid, doc, &filter, code)) return false;
      JsonArrayConst settings = doc["profileUsers"][0]["settings"];
      for (JsonVariantConst s : settings) {
        const char* id = s["id"] | "";
        if (!strcmp(id, "Gamerscore")) d.gamerscore = (uint32_t)strtoul(s["value"] | "0", nullptr, 10);
        else if (!strcmp(id, "Gamertag") && (s["value"] | "")[0]) strlcpy(d.gamertag, s["value"] | "", sizeof(d.gamertag));
      }
      return true;
    }

    // The title history comes newest first. Its achievement count only moves when something unlocks, and only then
    // is that game's list fetched to find the newest unlock.
    void fetchAchievement(const AppConfig& cfg, Data& d) {
      char titleId[16] = "", game[40] = "";
      uint16_t count = 0;
      {
        JsonDocument filter;
        filter["titles"][0]["titleId"] = true;
        filter["titles"][0]["name"] = true;
        filter["titles"][0]["achievement"]["currentAchievements"] = true;
        filter["titles"][0]["titleHistory"]["lastTimePlayed"] = true;
        JsonDocument doc(psramAllocator());
        int code;
        if (!call(cfg, String("/achievements/player/") + xuid, doc, &filter, code)) return;
        time_t best = 0;
        JsonArrayConst titles = doc["titles"];
        for (JsonVariantConst t : titles) {
          const time_t played = parseIso(t["titleHistory"]["lastTimePlayed"] | "");
          if ((t["achievement"]["currentAchievements"] | 0) == 0 || played <= best) continue;
          best = played;
          strlcpy(titleId, t["titleId"] | "", sizeof(titleId));
          strlcpy(game, t["name"] | "", sizeof(game));
          count = t["achievement"]["currentAchievements"] | 0;
        }
      }
      if (!titleId[0]) return;
      if (!strcmp(titleId, lastTitleId) && count == lastTitleCount && d.achievement[0]) return;   // nothing new
      JsonDocument filter;
      filter["achievements"][0]["name"] = true;
      filter["achievements"][0]["progressState"] = true;
      filter["achievements"][0]["progression"]["timeUnlocked"] = true;
      JsonDocument doc(psramAllocator());
      int code;
      if (!call(cfg, String("/achievements/player/") + xuid + "/" + titleId, doc, &filter, code)) return;
      time_t best = 0;
      JsonArrayConst list = doc["achievements"];
      for (JsonVariantConst a : list) {
        if (strcmp(a["progressState"] | "", "Achieved")) continue;
        const time_t t = parseIso(a["progression"]["timeUnlocked"] | "");
        if (t <= best) continue;
        best = t;
        strlcpy(d.achievement, a["name"] | "", sizeof(d.achievement));
      }
      if (!best) return;
      d.achievement_t = best;
      strlcpy(d.achievement_game, game, sizeof(d.achievement_game));
      strlcpy(lastTitleId, titleId, sizeof(lastTitleId));
      lastTitleCount = count;
    }

    void banners(const AppConfig& cfg, const Data& before, const Data& after) {
      if (!cfg.xbox.banners || !polledOnce) return;
      char line[128];
      if (after.online && !before.online) {
        if (after.game[0]) snprintf(line, sizeof(line), "%s is playing %s", after.gamertag, after.game);
        else snprintf(line, sizeof(line), "%s is online", after.gamertag);
        renderer::showMessage(line, BANNER_MS, BANNER_RGB);
        LOGI("xbox: %s", line);
      }
      if (after.achievement_t > before.achievement_t && before.achievement_t) {
        snprintf(line, sizeof(line), "%s unlocked %s (%s)", after.gamertag, after.achievement, after.achievement_game);
        renderer::showMessage(line, BANNER_MS, BANNER_RGB);
        LOGI("xbox: %s", line);
      }
    }
  }

  void begin() { mtx = xSemaphoreCreateMutex(); nextFast = nextSlow = millis() + FIRST_MS; }
  void applyConfig() { resetReq = true; net_task::kick(net_task::JOB_XBOX); }
  void requestRefresh() { refreshReq = true; net_task::kick(net_task::JOB_XBOX); }

  bool due(const AppConfig& cfg, uint32_t now) {
    if (!cfg.xbox.enabled || !cfg.xbox.api_key[0] || !cfg.xbox.gamertag[0]) return false;
    if (refreshReq || resetReq) return true;
    return (int32_t)(now - nextFast) >= 0 || (int32_t)(now - nextSlow) >= 0;
  }

  void run(const AppConfig& cfg) {
    st.enabled = cfg.xbox.enabled && cfg.xbox.api_key[0] && cfg.xbox.gamertag[0];
    if (resetReq) {
      resetReq = false;
      xuid[0] = lastTitleId[0] = '\0';
      lastTitleCount = 0xFFFF;
      polledOnce = false;
      if (take()) { data = Data(); st.valid = false; st.xuid[0] = '\0'; st.err[0] = '\0'; give(); }
      nextFast = nextSlow = millis();
    }
    if (!st.enabled || !wifi_mgr::isConnected()) return;
    const uint32_t now = millis();
    const bool force = refreshReq;
    refreshReq = false;
    if (!xuid[0] && !lookup(cfg)) { nextFast = nextSlow = now + RETRY_MS; return; }

    Data before; get(before);
    Data d = before;
    if (!d.gamertag[0]) strlcpy(d.gamertag, cfg.xbox.gamertag, sizeof(d.gamertag));
    const bool doFast = force || (int32_t)(now - nextFast) >= 0 || !before.valid;
    const bool doSlow = force || (int32_t)(now - nextSlow) >= 0 || !before.valid;
    bool ok = true;
    if (doFast) {
      ok = fetchPresence(cfg, d);
      if (ok) fetchFriends(cfg, d);
      nextFast = now + (ok ? FAST_MS : RETRY_MS);
    }
    if (doSlow && ok) {
      ok = fetchAccount(cfg, d);
      if (ok) fetchAchievement(cfg, d);
      nextSlow = now + (ok ? SLOW_MS : RETRY_MS);
    }
    if (!ok) return;
    d.valid = true;
    banners(cfg, before, d);
    polledOnce = true;
    if (take()) { data = d; st.valid = true; st.last_ok_ms = millis(); st.fails = 0; st.err[0] = '\0'; give(); }
    LOGI("xbox: %s %s%s%s, %lu G, %d friends on", d.gamertag, d.online ? "online" : "offline", d.game[0] ? " in " : "", d.game,
         (unsigned long)d.gamerscore, (int)d.friends_online);
  }

  Status status() { Status c = {}; if (take()) { c = st; give(); } return c; }
  bool get(Data& out) { if (!take()) return false; out = data; give(); return out.valid; }
}
