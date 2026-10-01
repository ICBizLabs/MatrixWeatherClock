#pragma once
#include <Arduino.h>
#include <time.h>

// Colour themes. Two kinds live in one shape:
//   * holidays, chosen automatically by the local date (and individually switchable), each with a decoration
//   * palettes, which you pick yourself in the web UI and which have no decoration
// A theme carries all six display colours, so a theme recolours the whole panel rather than just the clock.
namespace themes {
  enum class Deco : uint8_t { None, Snow, Confetti, Hearts, Sparkle };

  struct Theme {
    const char* id;                            // stable key stored in config, e.g. "christmas"
    const char* name;                          // shown in the web UI
    uint32_t time, date, temp, text, hi, lo;   // RGB888, matching ColorsConfig field for field
    Deco deco;
    bool holiday;                              // true = picked by date; false = a palette you choose
  };

  // Append to these tables only. display.holidays_enabled is a bitmask indexed by position in HOLIDAYS, so
  // inserting or reordering would silently remap which holidays somebody had switched off -- the same rule
  // actions::Id and PAGE_NAMES already follow.
  extern const Theme HOLIDAYS[];
  extern const size_t HOLIDAY_COUNT;
  extern const Theme PALETTES[];
  extern const size_t PALETTE_COUNT;

  const Theme* byId(const char* id);           // searches both tables; nullptr when unknown
  bool holidayEnabled(uint8_t i);              // consults display.holidays_enabled
  const Theme* forDate(const struct tm& lt);   // nullptr when today is not a holiday, or it is switched off
}
