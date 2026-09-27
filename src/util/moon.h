#pragma once
#include <Arduino.h>
#include <time.h>

// Moon phase from the date alone: the mean synodic month counted from a reference new moon, plus a small correction
// for the moon's elliptical orbit, which keeps the result within a few hours of the almanac.
namespace moon {
  enum class Phase : uint8_t { New = 0, WaxingCrescent, FirstQuarter, WaxingGibbous, Full, WaningGibbous, LastQuarter, WaningCrescent };
  struct Info {
    float age_days;          // 0 .. 29.53 since the new moon
    float fraction;          // 0 .. 1 through the cycle (0.5 = full)
    float illumination;      // 0 .. 1 of the disc lit
    Phase phase;
    bool waxing;
    float days_to_full;      // from now; 0 when it is full today
    float days_to_new;
  };
  Info at(time_t utc);
  const char* name(Phase p);          // "Waxing gibbous"
  const char* shortName(Phase p);     // "WAX GIBBOUS" for the panel
}
