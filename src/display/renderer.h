#pragma once
#include <Arduino.h>
#include "canvas.h"

// Screen state machine: splash, composite clock + bottom-half pages (with slide transitions, effects and holiday
// themes), forecast and hourly full screens, alert banner, messages, alarm/timer, lightning, test pattern, OTA.
namespace renderer {
  void begin(uint32_t now_ms);
  void applyDisplay();                        // after a display config change
  void tick(Canvas& c, uint32_t now_ms);      // draws one frame and keeps panel brightness up to date
  // Square LCD: a second 64x32 area under the clock. Once enabled, the forecast, hourly graph, world clock and radar
  // cycle there instead of replacing the clock. Call tickLower() right after tick() with its own canvas.
  void enableLower();
  void tickLower(Canvas& c, uint32_t now_ms);
  bool stepLower(int8_t dir);                 // +1 / -1: show the next / previous lower-half screen now
  void requestTest(uint32_t hold_ms);
  bool requestFullScreen(const char* name);   // "forecast", "hourly" (two page periods) or "radar" (radar.show_sec)
  const char* fullScreenBlockReason();        // "" when the periodic full screens can appear, else why not
  void setOta(bool active, uint8_t pct);
  void showIp(uint32_t hold_ms);
  void showMessage(const char* text, uint32_t hold_ms, uint32_t rgb);   // hold_ms 0 = until cleared
  void clearMessage();
  bool hasMessage();
  String messageText();
  uint32_t messageRemainingSec();
  void nextPage();
  void prevPage();
  void setDemo(bool on, uint32_t total_ms = 10 * 60000UL, bool sound = false, uint8_t start = 0);   // cycle demo scenarios from index `start` (synthetic data), auto-off
  bool demoActive();
  bool demoSound();
  bool consumeDemoSound(uint8_t& style, const char*& phrase);   // true when the demo wants a sound now: chime (ChimeStyle value, None = no chime) and/or a spoken phrase (nullptr = none)
  const char* demoScenario();
  uint32_t demoRemainingSec();
  void adjustBrightness(int8_t steps);        // remote: +-16 per step on top of the schedule, clamped
  int8_t brightnessOffset();
  void cycleNightOverride();                  // auto -> on -> off -> auto
  uint8_t nightOverride();                    // 0 auto, 1 forced on, 2 forced off
  bool nightActive();
  uint8_t effectiveBrightness();
  // Sleep timer: run normally for `minutes`, fade out over display.sleep_fade_sec, then hold the panel dark until
  // cancelled. 0 minutes cancels. Survives nothing but a reboot, by design.
  void startSleep(uint32_t minutes);
  void cancelSleep();
  bool sleepPending();                        // armed, counting down or faded out
  uint32_t sleepRemainingSec();               // until the fade starts; 0 once it has
  bool sleepFadedOut();
  const char* screenName();
  const char* themeName();                    // active holiday theme or ""
}
