#pragma once
#include <Arduino.h>

// The things a remote control (infrared, phone page, API) can do. One list shared by every input.
// Only ever append before COUNT: learned infrared buttons are stored by name, but DEFS[] in actions.cpp is sized
// from COUNT, so an entry added here without one there compiles cleanly and then hands strcmp a null pointer.
namespace actions {
  enum class Id : uint8_t {
    None = 0, NextPage, Dismiss, ShowRadar, ShowForecast, ShowHourly, AckAlerts, AlarmStop, AlarmSnooze,
    Timer5, Timer10, Timer30, TimerCancel, BrightUp, BrightDown, Night, Mute, Demo, Refresh, ShowIp, Chime,
    SayTime, StopwatchToggle, StopwatchReset, Sleep30, Sleep60, SleepCancel, ShowWorld, COUNT
  };
  const char* name(Id id);                 // "next_page"
  const char* label(Id id);                // "Next page"
  bool parse(const char* s, Id& out);
  bool run(Id id, const char* source);     // executes now; false for None / unknown
}
