#include "themes.h"
#include "config/config.h"
#include <string.h>

namespace themes {
  // id, name, time, date, temp, text, hi, lo, deco, holiday
  // hi stays the warmer colour of each pair and lo the cooler one, so a high and a low still read as warm and cool
  // whatever theme is on.
  const Theme HOLIDAYS[] = {
    { "new_year",     "New Year",       0xFFD700, 0xFFFFFF, 0xFFD700, 0xC0C0FF, 0xFFE680, 0xC0C0FF, Deco::Confetti, true },
    { "valentine",    "Valentine's",    0xFF4070, 0xFF90B0, 0xFF90B0, 0xFFC0D0, 0xFF6080, 0xFFC0D0, Deco::Hearts,   true },
    { "st_patrick",   "St. Patrick's",  0x30E060, 0xFFD700, 0xFFD700, 0x80FF80, 0x80FF80, 0x30E060, Deco::Sparkle,  true },
    { "easter",       "Easter",         0xC0A0FF, 0xFFE080, 0xFFE080, 0xA0FFC0, 0xFFB0C0, 0xA0FFC0, Deco::Sparkle,  true },
    { "july4",        "Independence",   0xFFFFFF, 0xFF3030, 0xFFFFFF, 0x4060FF, 0xFF3030, 0x4060FF, Deco::Confetti, true },
    { "halloween",    "Halloween",      0xFF7000, 0xA040FF, 0xFF7000, 0xFFB050, 0xFFB050, 0xA040FF, Deco::Sparkle,  true },
    { "thanksgiving", "Thanksgiving",   0xFFA030, 0xC06020, 0xFFA030, 0xFFD080, 0xFFD080, 0xC06020, Deco::None,     true },
    { "christmas",    "Christmas",      0xFF3030, 0x30D040, 0xFFFFFF, 0xFFFFFF, 0xFF3030, 0x30D040, Deco::Snow,     true },
    { "nye",          "New Year's Eve", 0xFFD700, 0xFFFFFF, 0xFFD700, 0xC0C0FF, 0xFFE680, 0xC0C0FF, Deco::Confetti, true },
  };
  const size_t HOLIDAY_COUNT = sizeof(HOLIDAYS) / sizeof(HOLIDAYS[0]);

  const Theme PALETTES[] = {
    { "default", "Default",       0xFFFFFF, 0x80C0FF, 0xFFD060, 0xC0C0C0, 0xFF8060, 0x60A0FF, Deco::None, false },
    { "mono",    "Mono",          0xFFFFFF, 0xB0B0B0, 0xE0E0E0, 0xC0C0C0, 0xFFFFFF, 0x909090, Deco::None, false },
    { "amber",   "Amber",         0xFFB000, 0xFF8C00, 0xFFC040, 0xD08000, 0xFFD060, 0xC06000, Deco::None, false },
    { "ocean",   "Ocean",         0xE0FFFF, 0x40C0FF, 0x60E0D0, 0xA0D0E0, 0x80FFE0, 0x3080FF, Deco::None, false },
    { "sunset",  "Sunset",        0xFFE0B0, 0xFF8040, 0xFFC070, 0xE0A080, 0xFF6030, 0xC040A0, Deco::None, false },
    { "forest",  "Forest",        0xE0FFD0, 0x60D060, 0xC0E080, 0xA0C090, 0xFFD060, 0x40A080, Deco::None, false },
    { "neon",    "Neon",          0x00FFFF, 0xFF00FF, 0xFFFF00, 0x80FFC0, 0xFF4080, 0x4080FF, Deco::None, false },
    { "night",   "Night (red)",   0xFF4000, 0xC02000, 0xFF6020, 0x902000, 0xFF5000, 0x802000, Deco::None, false },
  };
  const size_t PALETTE_COUNT = sizeof(PALETTES) / sizeof(PALETTES[0]);

  namespace {
    // Anonymous Gregorian algorithm (Meeus/Jones/Butcher): month (1-12) and day of Easter Sunday
    void easter(int year, int& month, int& day) {
      int a = year % 19, b = year / 100, c = year % 100, d = b / 4, e = b % 4, f = (b + 8) / 25, g = (b - f + 1) / 3;
      int h = (19 * a + b - d - g + 15) % 30, i = c / 4, k = c % 4, l = (32 + 2 * e + 2 * i - h - k) % 7;
      int m = (a + 11 * h + 22 * l) / 451;
      month = (h + l - 7 * m + 114) / 31;
      day = ((h + l - 7 * m + 114) % 31) + 1;
    }

    // One of your own dates, filled in on the fly. Only the clock and the date take your chosen colour; the rest of
    // the panel keeps the colours you configured, which is what a personal date should do.
    Theme custom = { "custom", "custom", 0xFFFFFF, 0xFFFFFF, 0xFFD060, 0xC0C0C0, 0xFF8060, 0x60A0FF, Deco::Sparkle, true };

    // Index into HOLIDAYS, so the bitmask keeps its meaning. Must match the order of the table above.
    enum : uint8_t { H_NEW_YEAR, H_VALENTINE, H_ST_PATRICK, H_EASTER, H_JULY4, H_HALLOWEEN, H_THANKS, H_CHRISTMAS, H_NYE };

    const Theme* pick(uint8_t i) { return holidayEnabled(i) ? &HOLIDAYS[i] : nullptr; }
  }

  const Theme* byId(const char* id) {
    if (!id || !*id) return nullptr;
    for (size_t i = 0; i < HOLIDAY_COUNT; i++) if (strcmp(HOLIDAYS[i].id, id) == 0) return &HOLIDAYS[i];
    for (size_t i = 0; i < PALETTE_COUNT; i++) if (strcmp(PALETTES[i].id, id) == 0) return &PALETTES[i];
    return nullptr;
  }

  bool holidayEnabled(uint8_t i) {
    if (i >= HOLIDAY_COUNT || i >= 32) return false;
    return (g_cfg.display.holidays_enabled >> i) & 1u;
  }

  const Theme* forDate(const struct tm& lt) {
    const int mon = lt.tm_mon + 1, day = lt.tm_mday, year = lt.tm_year + 1900;
    // Your own dates win over the built-in list, so a birthday can override a holiday on the same day. They are not
    // affected by the per-holiday switches, which only govern the built-ins.
    for (uint8_t i = 0; i < g_cfg.display.nholidays && i < MAX_CUSTOM_HOLIDAYS; i++) {
      const HolidayConfig& h = g_cfg.display.holidays[i];
      if (h.month != mon || h.day != day) continue;
      const ColorsConfig& c = g_cfg.display.colors;
      custom.name = h.label[0] ? h.label : "custom";
      custom.time = h.color;
      custom.date = h.color;
      custom.temp = c.temp;
      custom.text = c.text;
      custom.hi = c.hi;
      custom.lo = c.lo;
      custom.deco = Deco::Sparkle;
      return &custom;
    }
    if (mon == 1 && day == 1) return pick(H_NEW_YEAR);
    if (mon == 2 && day == 14) return pick(H_VALENTINE);
    if (mon == 3 && day == 17) return pick(H_ST_PATRICK);
    if (mon == 7 && day == 4) return pick(H_JULY4);
    if (mon == 10 && day == 31) return pick(H_HALLOWEEN);
    if (mon == 11 && lt.tm_wday == 4 && day >= 22 && day <= 28) return pick(H_THANKS);   // fourth Thursday
    if (mon == 12 && (day == 24 || day == 25)) return pick(H_CHRISTMAS);
    if (mon == 12 && day == 31) return pick(H_NYE);
    int em, ed;
    easter(year, em, ed);
    if (mon == em && day == ed) return pick(H_EASTER);
    return nullptr;
  }
}
