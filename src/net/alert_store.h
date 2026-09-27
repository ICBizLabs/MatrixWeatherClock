#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <time.h>
#include "config/config.h"

// Active NWS alerts with their lifecycle (new -> flashing/chimed -> acknowledged -> expired/cancelled).
struct AlertItem {
  char id[80];
  char event[48];
  char headline[160];
  char sender[40];
  char area[96];
  char msgType[8];          // Alert / Update / Cancel
  char ref[80];             // identifier of the alert this one updates (first reference), empty if none
  Severity sev;
  uint8_t urgency;          // 0 unknown, 1 past, 2 future, 3 expected, 4 immediate
  time_t onset, ends, expires;
  uint32_t first_seen_ms;
  uint32_t last_chime_ms;
  bool chimed, acknowledged, synthetic, notified;
};

constexpr size_t MAX_ALERTS = 12;

// Compact view for the renderer (severity-sorted, filtered)
struct AlertViewItem {
  char event[48];
  char headline[160];
  Severity sev;
  uint32_t first_seen_ms;
  bool acknowledged;
};
struct AlertView {
  uint8_t n = 0;
  AlertViewItem items[8];
  bool stale = false;
  Severity top = Severity::Unknown;
  uint32_t newest_ms = 0;       // first_seen of the most recently seen unacknowledged alert
};

namespace alerts {
  void begin();
  void applySnapshot(const AlertItem* fresh, size_t n, uint32_t now_ms);   // after a successful poll (net task)
  void pollFailed();
  void expire(time_t now_utc);                                              // drop ended alerts (loop, once a second)
  void injectTest(const char* event, Severity s, const char* headline, uint16_t minutes, uint32_t now_ms);
  void acknowledge(const char* id_or_all);
  void view(const AlertsConfig& cfg, AlertView& out);
  // true once per (re)chime; top = highest severity that fired, event = its event name (for the spoken announcement)
  bool takeNewForChime(const AlertsConfig& cfg, uint16_t repeat_min, uint32_t now_ms, Severity* top = nullptr, char* event = nullptr, size_t eventLen = 0);
  // One un-notified alert at or above `min`. sevOut reports its severity so several sinks with different
  // thresholds can share the single consume.
  bool takeNewForNotify(const AlertsConfig& cfg, Severity min, char* event, size_t eventLen, char* headline, size_t headlineLen, Severity* sevOut = nullptr);
  size_t count();
  bool stale();
  void toJson(JsonArray arr, const AlertsConfig& cfg, bool all);
  bool passesFilter(const AlertItem& a, const AlertsConfig& cfg);
}
