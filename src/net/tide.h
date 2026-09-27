#pragma once
#include <Arduino.h>
#include <time.h>
#include "config/config.h"

// High and low tide predictions from NOAA CO-OPS (tidesandcurrents.noaa.gov) for one station: the next two days of
// extremes, refreshed a few times a day. The station is chosen in the web UI (nearest-station search runs in the
// browser). Times arrive as station local time and are kept as epoch seconds via the clock's own time zone.
namespace tide {
  struct Extreme { time_t t = 0; float h = 0; bool high = false; };
  constexpr uint8_t MAX_EXTREMES = 12;
  struct Data {
    bool valid = false;
    bool metric = false;             // heights in metres, else feet (datum MLLW)
    uint32_t fetched_ms = 0;
    char station[12] = "";
    uint8_t n = 0;
    Extreme ex[MAX_EXTREMES];        // ascending time
    float water_temp = -999;         // latest water temperature at the station, display unit; -999 = not reported
    time_t water_t = 0;              // when that reading was taken
  };
  struct Status {
    bool enabled;
    bool valid;
    uint32_t last_ok_ms, last_err_ms;
    uint8_t fails;
    char err[48];
  };
  void begin();
  void applyConfig();                          // CHG_TIDE / CHG_LOCATION: forget and refetch
  void requestRefresh();
  bool due(const AppConfig& cfg, uint32_t now_ms);
  void run(const AppConfig& cfg);              // network task
  Status status();
  bool get(Data& out);                         // copy of the current data; returns out.valid
  // From the extremes around `now`: the next high and low, whether the tide is rising, and the interpolated height
  // (cosine between the two neighbouring extremes, the usual rule-of-twelfths shape). false = no data around now.
  bool now(const Data& d, time_t now, Extreme& nextHigh, Extreme& nextLow, bool& rising, float& height);
}
