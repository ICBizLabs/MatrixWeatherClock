#pragma once
#include <Arduino.h>
#include "config/config.h"

// Application-level glue: config staging from web handlers, reboot/reset requests, config mutex.
namespace app {
  void begin();
  void loop();                                              // applies staged config, executes reboot / factory reset
  void cfgLock();
  void cfgUnlock();
  bool stageConfig(const AppConfig& next, uint32_t changed); // from async web handlers; applied on the main loop
  void requestReboot(uint32_t delay_ms);
  void requestFactoryReset();
  bool rebootPending();
  uint32_t uptimeSec();
  uint32_t rebootRequiredFlags();                            // CHG_* bits that need a reboot to take effect

  // Crash black box: the last activity is kept in RTC memory (survives resets, not power loss) and reported at boot.
  struct LastReset { bool valid = false; int reason = 0; char where[40] = ""; uint32_t uptime_s = 0; uint32_t heap = 0; };
  void trace(const char* what, const char* detail = nullptr);   // cheap; call from any task
  LastReset lastReset();
  const char* resetReasonName(int reason);
}
