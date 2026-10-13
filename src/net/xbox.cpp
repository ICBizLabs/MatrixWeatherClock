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
    char xuid[24] = "";                       // Xbox user id for the gamertag, looked up once; every other request needs it
    uint32_t nextFast = 0, nextSlow = 0, hourStart = 0;
    bool polledOnce = false;                   // banners only after the first full poll, so a boot stays quiet
    volatile bool refreshReq = false, resetReq = false;   // set from the web task, acted on in the network task

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

    // GET a path under the API into doc, keeping only what filter names (filter may be null for small replies).
    // A big reply (a long game history, a busy player's friends list) can break off part way; with partial set, what
    // arrived before the break is used, which suits a list that comes newest first.
    bool call(const AppConfig& cfg, const String& path, JsonDocument& doc, const JsonDocument* filter, int& code,
              bool partial = false) {
      http_util::Options opt;
      opt.userAgent = MWC_USER_AGENT_NAME "/" MWC_VERSION;
      opt.accept = "application/json";
      opt.timeoutMs = 15000;
      opt.headerName = "X-Authorization";
      opt.headerValue = cfg.xbox.api_key;
      const uint32_t now = millis();                    // count requests per hour for the Status tab (free tier: 150)
      if (now - hourStart >= 3600000UL) { hourStart = now; st.requests_hour = 0; }
      st.requests_hour++;
      String err;
      code = 0;
      // OpenXBL wraps the Xbox Live reply in {"content": ...}; the filter allows both, and the wrapper is dropped
      DeserializationError jerr = DeserializationError::Ok;
      JsonDocument wrapped;
      if (filter) { wrapped.set(*filter); wrapped["content"] = *filter; }
      bool ok = http_util::get(String(API) + path, opt, [&](Stream& s, int len) {
        DeserializationError e = filter ? deserializeJson(doc, s, DeserializationOption::Filter(wrapped))
                                        : deserializeJson(doc, s);
        jerr = e;
        return e == DeserializationError::Ok || (partial && e == DeserializationError::IncompleteInput && doc.size());
      }, err, &code);
      if (ok && jerr != DeserializationError::Ok) LOGI("xbox: %s broke off, using the part that arrived", path.c_str());
      if (!ok && jerr != DeserializationError::Ok) err = String("reply not read: ") + jerr.c_str();
      if (ok && !doc["content"].isNull()) { JsonDocument inner(psramAllocator()); inner.set(doc["content"]); doc = std::move(inner); }
      if (!ok) {
        if (code == 429) err = "rate limited (150 requests an hour)";
        else if (code == 401 || code == 403) err = "API key refused";
        fail(err.c_str());
      }
      return ok;
    }

    // gamertag -> XUID, also picking up the gamertag as Xbox spells it. The search answers in one of two shapes:
    // a profile like /account (profileUsers[0].id) or a people list (people[0].xuid); both are accepted.
    bool lookup(const AppConfig& cfg) {
      JsonDocument doc(psramAllocator());      // a small reply (about 1.4 KB), so it is read whole
      int code;
      if (!call(cfg, String("/friends/search?gt=") + urlEncode(cfg.xbox.gamertag), doc, nullptr, code)) return false;
      const char* x = doc["profileUsers"][0]["id"] | (doc["people"][0]["xuid"] | "");
      if (!x[0]) {
        String body; serializeJson(doc, body);
        LOGW("xbox: search for %s answered %.200s", cfg.xbox.gamertag, body.c_str());
        fail("gamertag not found");
        return false;
      }
      strlcpy(xuid, x, sizeof(xuid));
      const char* gt = doc["people"][0]["gamertag"] | cfg.xbox.gamertag;
      JsonArrayConst settings = doc["profileUsers"][0]["settings"];
      for (JsonVariantConst s : settings) if (!strcmp(s["id"] | "", "Gamertag") && (s["value"] | "")[0]) gt = s["value"];
      if (!gt[0]) gt = cfg.xbox.gamertag;
      if (take()) { strlcpy(st.xuid, xuid, sizeof(st.xuid)); strlcpy(data.gamertag, gt, sizeof(data.gamertag)); give(); }
      LOGI("xbox: %s is xuid %s", cfg.xbox.gamertag, xuid);
      return true;
    }

    // Online state and the foreground title. The reply is small, so it is parsed whole. It may be an object or a
    // one-element array (client libraries show both), so either is accepted. The home screen is not a game.
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

    // gamerscore (and the gamertag spelling) from the profile settings list
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

    // The newest unlocks from the three most recently played games that have achievements. A game's achievement
    // count only moves when something unlocks, and only then is that game's list fetched again, so a quiet poll
    // costs the one title-history request.
    constexpr uint8_t MAX_TITLES = 3;
    struct TitleCache {
      char id[16] = "";
      char game[40] = "";
      uint16_t count = 0;
      uint8_t n = 0;
      struct { char name[48]; time_t t; } a[Data::MAX_RECENT];
    };
    TitleCache tcache[MAX_TITLES];

    // one game's newest unlocks into tc (false when the request fails)
    bool fetchTitle(const AppConfig& cfg, TitleCache& tc) {
      JsonDocument filter;
      filter["achievements"][0]["name"] = true;
      filter["achievements"][0]["progressState"] = true;
      filter["achievements"][0]["progression"]["timeUnlocked"] = true;
      JsonDocument doc(psramAllocator());
      int code;
      if (!call(cfg, String("/achievements/player/") + xuid + "/" + tc.id, doc, &filter, code, true)) return false;
      tc.n = 0;
      JsonArrayConst list = doc["achievements"];
      for (JsonVariantConst a : list) {
        if (strcmp(a["progressState"] | "", "Achieved")) continue;
        const time_t t = parseIso(a["progression"]["timeUnlocked"] | "");
        if (!t) continue;
        // insertion into a short list kept newest first
        uint8_t k = tc.n < Data::MAX_RECENT ? tc.n++ : Data::MAX_RECENT;
        if (k == Data::MAX_RECENT) { if (t <= tc.a[k - 1].t) continue; k--; }
        while (k > 0 && tc.a[k - 1].t < t) { tc.a[k] = tc.a[k - 1]; k--; }
        strlcpy(tc.a[k].name, a["name"] | "", sizeof(tc.a[k].name));
        tc.a[k].t = t;
      }
      return true;
    }

    void fetchAchievements(const AppConfig& cfg, Data& d) {
      TitleCache next[MAX_TITLES];
      uint8_t nt = 0;
      {
        JsonDocument filter;
        filter["titles"][0]["titleId"] = true;
        filter["titles"][0]["name"] = true;
        filter["titles"][0]["achievement"]["currentAchievements"] = true;
        filter["titles"][0]["titleHistory"]["lastTimePlayed"] = true;
        JsonDocument doc(psramAllocator());
        int code;
        if (!call(cfg, String("/achievements/player/") + xuid, doc, &filter, code, true)) return;
        time_t played[MAX_TITLES] = {};
        JsonArrayConst titles = doc["titles"];
        for (JsonVariantConst t : titles) {       // keep the three most recently played, newest first
          const uint16_t count = t["achievement"]["currentAchievements"] | 0;
          const time_t p = parseIso(t["titleHistory"]["lastTimePlayed"] | "");
          if (!count || !p) continue;
          uint8_t k = nt < MAX_TITLES ? nt++ : MAX_TITLES;
          if (k == MAX_TITLES) { if (p <= played[k - 1]) continue; k--; }
          while (k > 0 && played[k - 1] < p) { next[k] = next[k - 1]; played[k] = played[k - 1]; k--; }
          next[k] = TitleCache();
          strlcpy(next[k].id, t["titleId"] | "", sizeof(next[k].id));
          strlcpy(next[k].game, t["name"] | "", sizeof(next[k].game));
          next[k].count = count;
          played[k] = p;
        }
      }
      for (uint8_t i = 0; i < nt; i++) {
        const TitleCache* old = nullptr;
        for (const TitleCache& c : tcache) if (c.id[0] && !strcmp(c.id, next[i].id)) old = &c;
        if (old && old->count == next[i].count) { next[i] = *old; continue; }   // nothing new in this game
        TitleCache fresh = next[i];
        if (fetchTitle(cfg, fresh)) next[i] = fresh;
        else if (old) next[i] = *old;                                               // keep what we had; retry next poll
        else next[i].count = 0;                                                     // forces a retry next poll
      }
      for (uint8_t i = 0; i < MAX_TITLES; i++) tcache[i] = i < nt ? next[i] : TitleCache();
      // merge the games' lists into the newest MAX_RECENT overall
      d.nrecent = 0;
      for (const TitleCache& c : tcache) {
        for (uint8_t j = 0; j < c.n; j++) {
          const time_t t = c.a[j].t;
          uint8_t k = d.nrecent < Data::MAX_RECENT ? d.nrecent++ : Data::MAX_RECENT;
          if (k == Data::MAX_RECENT) { if (t <= d.recent[k - 1].t) continue; k--; }
          while (k > 0 && d.recent[k - 1].t < t) { d.recent[k] = d.recent[k - 1]; k--; }
          strlcpy(d.recent[k].name, c.a[j].name, sizeof(d.recent[k].name));
          strlcpy(d.recent[k].game, c.game, sizeof(d.recent[k].game));
          d.recent[k].t = t;
        }
      }
    }

    // Scroll a line when the player comes online or a newer achievement appears. Not on the first poll after boot
    // or a gamertag change, which would announce whatever state the player happens to be in.
    void banners(const AppConfig& cfg, const Data& before, const Data& after) {
      if (!cfg.xbox.banners || !polledOnce) return;
      char line[128];
      if (after.online && !before.online) {
        if (after.game[0]) snprintf(line, sizeof(line), "%s is playing %s", after.gamertag, after.game);
        else snprintf(line, sizeof(line), "%s is online", after.gamertag);
        renderer::showMessage(line, BANNER_MS, BANNER_RGB);
        LOGI("xbox: %s", line);
      }
      // a newer unlock than before, and a recent one: a game whose list only now loaded is not news
      if (after.nrecent && (!before.nrecent || after.recent[0].t > before.recent[0].t) && after.recent[0].t > time(nullptr) - 86400) {
        snprintf(line, sizeof(line), "%s unlocked %s (%s)", after.gamertag, after.recent[0].name, after.recent[0].game);
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
    if (resetReq) {                            // settings changed: forget the player and start over
      resetReq = false;
      xuid[0] = '\0';
      for (auto& c : tcache) c = TitleCache();
      polledOnce = false;
      if (take()) { data = Data(); st.valid = false; st.xuid[0] = '\0'; st.err[0] = '\0'; give(); }
      nextFast = nextSlow = millis();
    }
    if (!st.enabled || !wifi_mgr::isConnected()) return;
    const uint32_t now = millis();
    const bool force = refreshReq;
    refreshReq = false;
    if (!xuid[0] && !lookup(cfg)) { nextFast = nextSlow = now + RETRY_MS; return; }

    // Fast items (presence, friends) and slow items (gamerscore, achievements) keep their own timers. Work on a copy
    // so the screen never sees a half-updated player, and compare it with the old one for the banners.
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
      if (ok) fetchAchievements(cfg, d);
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
