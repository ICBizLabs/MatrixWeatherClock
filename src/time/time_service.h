#pragma once
#include <Arduino.h>
#include <time.h>
#include "config/config.h"

// Wall clock: RTC at boot, NTP once WiFi is up, RTC refreshed after every NTP sync.
namespace timesvc {
  enum class Source : uint8_t { None, Rtc, Ntp };
  struct Status { Source source; time_t last_sync; bool rtc_present; };

  void begin(const TimeConfig& tc);
  void onWifiUp(const TimeConfig& tc);        // starts SNTP
  void applyTz(const TimeConfig& tc);         // live timezone / server change
  void loop();                                 // detects NTP syncs and writes the RTC
  bool valid();
  bool localNow(struct tm& lt, uint16_t* ms = nullptr);
  Status status();
  const char* sourceName(Source s);

  // The C library timezone is process-global, so anything that converts a time takes this guard. zoneNow() swaps TZ
  // to a second zone, reads the clock and swaps back, which is how the world-clock page works without a second
  // timezone database. Recursive, so nesting is safe.
  void tzLock();
  void tzUnlock();
  bool zoneNow(const char* posix, struct tm& out);   // false when posix is empty or the clock is not set yet
}
