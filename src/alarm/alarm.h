#pragma once
#include <Arduino.h>

// Alarm clock (up to MAX_ALARMS entries in the config) and a single countdown timer. Both "ring" by repeating the
// configured chime and taking over the bottom half of the display until stopped, snoozed or timed out.
namespace alarmclock {
  void begin();
  void loop(uint32_t now_ms);                 // call once per second
  bool ringing();
  bool ringingIsTimer();
  const char* ringingLabel();
  void stop();                                // stop the current ring (alarm or timer)
  void snooze();                              // alarms only: ring again after the alarm's snooze_min
  bool snoozed();
  uint32_t snoozeRemainingSec();
  bool startTimer(uint32_t seconds);          // 1 s .. 24 h
  void cancelTimer();
  bool timerRunning();
  uint32_t timerRemainingSec();

  // Stopwatch: counts up until stopped, holds the time until reset. Independent of the countdown timer.
  void stopwatchStart();
  void stopwatchStop();                       // pause, keeping the elapsed time
  void stopwatchReset();                      // back to zero and off the display
  void stopwatchToggle();                     // start / pause, for a remote button
  bool stopwatchRunning();
  bool stopwatchActive();                     // running, or paused with a time worth showing
  uint32_t stopwatchMs();

  // Hourly chime, driven from loop(): one chime, or the hour struck 1..12 times, plus an optional spoken time.
  bool striking();
}
