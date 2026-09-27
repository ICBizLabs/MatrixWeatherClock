#pragma once
#include <Arduino.h>
#include <time.h>

// Real-time lightning strikes from the Blitzortung.org community network via the public MQTT relay used by the
// Home Assistant integration (blitzortung.ha.sed.pl; personal use only). Runs in its own task on core 0.
namespace lightning {
  struct Status {
    bool enabled;
    bool connected;
    bool active;              // at least one strike inside the radius within the window
    uint16_t count;           // strikes in the window
    float nearest_km;         // closest strike in the window
    float latest_km;          // most recent strike
    int16_t latest_bearing;   // degrees from the clock's location to the strike
    time_t latest_time;       // epoch seconds
    uint32_t total;           // strikes accepted since boot
  };
  void begin();
  void applyConfig();            // config or location changed: resubscribe
  Status status();
  bool consumeStrikeEvent();     // true once for every new nearby strike (drives the flash animation)
  bool consumeChimeEvent();      // true when a chime is due (first strike of a storm, then rate limited)
  bool consumeNotifyEvent();     // same cadence, independent of the chime setting (phone notifications)
  bool consumeWebhookEvent();    // same cadence again, for the outgoing webhook
  const char* bearingName(int16_t deg);
}
