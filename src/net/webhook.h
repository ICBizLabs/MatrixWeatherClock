#pragma once
#include <Arduino.h>
#include "config/config.h"

// A plain HTTP POST to any URL when something happens: ntfy, Gotify, Home Assistant, Discord, a home-grown script.
// The body is a small JSON object whose two field names are configurable, so one shape fits several services:
//   { "<title_key>": "Tornado Warning", "<body_key>": "...", "event": "alert", "device": "matrixweatherclock" }
// Notifications are queued from any task and sent by the network task, exactly like pushbullet.
namespace webhook {
  enum class Event : uint8_t { Alert, Lightning, Alarm, Air, Boot, Test };
  struct Status { bool configured; uint32_t sent, errors; char last_err[48]; uint32_t last_ms; };
  void begin();
  bool notify(Event ev, const char* title, const char* body);   // false when not configured or this event is off
  bool wants(Event ev, Severity sev);                           // is this event enabled (severity only matters for alerts)
  void runQueued(const AppConfig& cfg);                         // network task
  Status status();
}
