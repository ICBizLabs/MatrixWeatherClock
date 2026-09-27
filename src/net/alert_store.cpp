#include "alert_store.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_heap_caps.h>
#include <strings.h>
#include "util/log.h"

namespace alerts {
  namespace {
    SemaphoreHandle_t mtx = nullptr;
    AlertItem* items = nullptr;   // PSRAM
    size_t count_ = 0;
    uint8_t failStreak = 0;
    constexpr uint8_t STALE_AFTER = 3;

    bool take() { return mtx && xSemaphoreTake(mtx, pdMS_TO_TICKS(200)) == pdTRUE; }
    void give() { xSemaphoreGive(mtx); }

    int findById(const char* id) {
      for (size_t i = 0; i < count_; i++) if (strcmp(items[i].id, id) == 0) return (int)i;
      return -1;
    }
    void removeAt(size_t i) {
      for (size_t j = i; j + 1 < count_; j++) items[j] = items[j + 1];
      count_--;
    }
    bool ignored(const char* event, const char* list) {
      if (!list || !*list) return false;
      char buf[192];
      strlcpy(buf, list, sizeof(buf));
      char* save = nullptr;
      for (char* tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(nullptr, ",", &save)) {
        while (*tok == ' ') tok++;
        size_t n = strlen(tok);
        while (n && tok[n - 1] == ' ') tok[--n] = '\0';
        if (n && strcasecmp(tok, event) == 0) return true;
      }
      return false;
    }
  }

  void begin() {
    mtx = xSemaphoreCreateMutex();
    items = (AlertItem*)heap_caps_calloc(MAX_ALERTS, sizeof(AlertItem), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!items) items = (AlertItem*)calloc(MAX_ALERTS, sizeof(AlertItem));
  }

  bool passesFilter(const AlertItem& a, const AlertsConfig& cfg) {
    if ((uint8_t)a.sev < (uint8_t)cfg.min_severity) return false;
    if (ignored(a.event, cfg.ignored_events)) return false;
    return true;
  }

  void applySnapshot(const AlertItem* fresh, size_t n, uint32_t now_ms) {
    if (!take()) return;
    failStreak = 0;
    // 1. drop stored real alerts that are no longer active (covers Cancel and server-side expiry)
    for (size_t i = 0; i < count_;) {
      bool present = false;
      if (!items[i].synthetic) {
        for (size_t k = 0; k < n; k++) if (strcmp(items[i].id, fresh[k].id) == 0) { present = true; break; }
      } else present = true;
      if (!present) { LOGI("alerts: gone %s (%s)", items[i].id, items[i].event); removeAt(i); }
      else i++;
    }
    // 2. merge fresh alerts
    for (size_t k = 0; k < n; k++) {
      const AlertItem& f = fresh[k];
      if (strcmp(f.msgType, "Cancel") == 0) continue;
      int idx = findById(f.id);
      if (idx >= 0) {
        AlertItem& it = items[idx];
        uint32_t fs = it.first_seen_ms, lc = it.last_chime_ms;
        bool ch = it.chimed, ack = it.acknowledged, nt = it.notified;
        it = f;
        it.first_seen_ms = fs; it.last_chime_ms = lc; it.chimed = ch; it.acknowledged = ack; it.notified = nt; it.synthetic = false;
        continue;
      }
      AlertItem nu = f;
      nu.synthetic = false;
      nu.first_seen_ms = now_ms;
      nu.last_chime_ms = 0;
      nu.chimed = false;
      nu.acknowledged = false;
      nu.notified = false;
      // an Update that references an alert we already had inherits its flags (no second flash / chime)
      if (nu.ref[0]) {
        int r = findById(nu.ref);
        if (r >= 0) {
          nu.first_seen_ms = items[r].first_seen_ms; nu.last_chime_ms = items[r].last_chime_ms;
          nu.chimed = items[r].chimed; nu.acknowledged = items[r].acknowledged; nu.notified = items[r].notified;
          removeAt(r);
        }
      }
      if (count_ >= MAX_ALERTS) {
        // evict the least severe stored alert if the new one is more severe
        size_t worst = 0;
        for (size_t i = 1; i < count_; i++) if ((uint8_t)items[i].sev < (uint8_t)items[worst].sev) worst = i;
        if ((uint8_t)items[worst].sev >= (uint8_t)nu.sev) continue;
        removeAt(worst);
      }
      items[count_++] = nu;
      LOGI("alerts: new %s [%s] %s", nu.event, severity_name(nu.sev), nu.headline);
    }
    give();
  }

  void pollFailed() {
    if (!take()) return;
    if (failStreak < 255) failStreak++;
    give();
  }

  void expire(time_t now_utc) {
    if (now_utc < 1700000000 || !take()) return;
    for (size_t i = 0; i < count_;) {
      time_t end = items[i].ends ? items[i].ends : items[i].expires;
      if (end && end < now_utc) { LOGI("alerts: expired %s", items[i].event); removeAt(i); }
      else i++;
    }
    give();
  }

  void injectTest(const char* event, Severity s, const char* headline, uint16_t minutes, uint32_t now_ms) {
    if (!take()) return;
    AlertItem t = {};
    snprintf(t.id, sizeof(t.id), "test-%lu", (unsigned long)now_ms);
    strlcpy(t.event, event && *event ? event : "Test Alert", sizeof(t.event));
    strlcpy(t.headline, headline && *headline ? headline : "This is a test of the matrix weather clock", sizeof(t.headline));
    strlcpy(t.sender, "test", sizeof(t.sender));
    strlcpy(t.msgType, "Alert", sizeof(t.msgType));
    t.sev = s;
    t.urgency = 4;
    time_t now = time(nullptr);
    t.onset = now;
    t.ends = t.expires = now + (time_t)minutes * 60;
    t.first_seen_ms = now_ms;
    t.synthetic = true;
    if (count_ >= MAX_ALERTS) removeAt(count_ - 1);
    items[count_++] = t;
    give();
    LOGI("alerts: injected test %s [%s]", t.event, severity_name(s));
  }

  void acknowledge(const char* id_or_all) {
    if (!take()) return;
    bool all = !id_or_all || !*id_or_all || strcmp(id_or_all, "all") == 0;
    for (size_t i = 0; i < count_; i++) if (all || strcmp(items[i].id, id_or_all) == 0) items[i].acknowledged = true;
    give();
  }

  void view(const AlertsConfig& cfg, AlertView& out) {
    out.n = 0;
    out.top = Severity::Unknown;
    out.newest_ms = 0;
    out.stale = failStreak >= STALE_AFTER;
    if (!take()) return;
    // collect indices passing the filter, sorted by severity descending then first_seen descending
    size_t idx[MAX_ALERTS]; size_t n = 0;
    for (size_t i = 0; i < count_; i++) if (passesFilter(items[i], cfg)) idx[n++] = i;
    for (size_t a = 1; a < n; a++) {
      size_t v = idx[a]; size_t b = a;
      while (b > 0 && ((uint8_t)items[idx[b - 1]].sev < (uint8_t)items[v].sev ||
             ((uint8_t)items[idx[b - 1]].sev == (uint8_t)items[v].sev && items[idx[b - 1]].first_seen_ms < items[v].first_seen_ms))) {
        idx[b] = idx[b - 1]; b--;
      }
      idx[b] = v;
    }
    for (size_t a = 0; a < n && out.n < 8; a++) {
      const AlertItem& it = items[idx[a]];
      AlertViewItem& o = out.items[out.n++];
      strlcpy(o.event, it.event, sizeof(o.event));
      strlcpy(o.headline, it.headline, sizeof(o.headline));
      o.sev = it.sev;
      o.first_seen_ms = it.first_seen_ms;
      o.acknowledged = it.acknowledged;
      if ((uint8_t)it.sev > (uint8_t)out.top) out.top = it.sev;
      if (!it.acknowledged && it.first_seen_ms > out.newest_ms) out.newest_ms = it.first_seen_ms;
    }
    give();
  }

  bool takeNewForChime(const AlertsConfig& cfg, uint16_t repeat_min, uint32_t now_ms, Severity* top, char* event, size_t eventLen) {
    if (!take()) return false;
    bool fire = false;
    Severity best = Severity::Unknown;
    int bestIdx = -1;
    for (size_t i = 0; i < count_; i++) {
      AlertItem& it = items[i];
      if (it.acknowledged || !passesFilter(it, cfg) || (uint8_t)it.sev < (uint8_t)cfg.chime_min_severity) continue;
      bool f = false;
      if (!it.chimed) { it.chimed = true; it.last_chime_ms = now_ms; f = true; }
      else if (repeat_min && now_ms - it.last_chime_ms >= (uint32_t)repeat_min * 60000UL) { it.last_chime_ms = now_ms; f = true; }
      if (!f) continue;
      fire = true;
      // the highest severity wins; among equals the most recently seen alert
      if (bestIdx < 0 || (uint8_t)it.sev > (uint8_t)best || ((uint8_t)it.sev == (uint8_t)best && it.first_seen_ms > items[bestIdx].first_seen_ms)) {
        best = it.sev;
        bestIdx = (int)i;
      }
    }
    if (event && eventLen) strlcpy(event, bestIdx >= 0 ? items[bestIdx].event : "", eventLen);
    give();
    if (top) *top = best;
    return fire;
  }

  bool takeNewForNotify(const AlertsConfig& cfg, Severity min, char* event, size_t eventLen, char* headline, size_t headlineLen, Severity* sevOut) {
    if (!take()) return false;
    bool found = false;
    for (size_t i = 0; i < count_ && !found; i++) {
      AlertItem& it = items[i];
      if (it.notified || !passesFilter(it, cfg) || (uint8_t)it.sev < (uint8_t)min) continue;
      it.notified = true;
      strlcpy(event, it.event, eventLen);
      strlcpy(headline, it.headline, headlineLen);
      if (sevOut) *sevOut = it.sev;
      found = true;
    }
    give();
    return found;
  }

  size_t count() { return count_; }
  bool stale() { return failStreak >= STALE_AFTER; }

  void toJson(JsonArray arr, const AlertsConfig& cfg, bool all) {
    if (!take()) return;
    for (size_t i = 0; i < count_; i++) {
      const AlertItem& it = items[i];
      if (!all && !passesFilter(it, cfg)) continue;
      JsonObject o = arr.add<JsonObject>();
      o["id"] = it.id;
      o["event"] = it.event;
      o["severity"] = severity_name(it.sev);
      o["urgency"] = it.urgency;
      o["headline"] = it.headline;
      o["sender"] = it.sender;
      o["area"] = it.area;
      o["type"] = it.msgType;
      o["onset"] = (long)it.onset;
      o["ends"] = (long)it.ends;
      o["expires"] = (long)it.expires;
      o["acknowledged"] = it.acknowledged;
      o["chimed"] = it.chimed;
      o["test"] = it.synthetic;
      o["shown"] = passesFilter(it, cfg);
      o["age_s"] = (uint32_t)((millis() - it.first_seen_ms) / 1000);
    }
    give();
  }
}
