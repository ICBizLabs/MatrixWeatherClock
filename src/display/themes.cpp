#include "themes.h"
#include "config/config.h"

namespace themes {
  namespace {
    const Theme NEW_YEAR   = { "New Year",       0xFFD700, 0xFFFFFF, 0xC0C0FF, Deco::Confetti };
    const Theme VALENTINE  = { "Valentine's",    0xFF4070, 0xFF90B0, 0xFFC0D0, Deco::Hearts };
    const Theme ST_PATRICK = { "St. Patrick's",  0x30E060, 0xFFD700, 0x80FF80, Deco::Sparkle };
    const Theme EASTER     = { "Easter",         0xC0A0FF, 0xFFE080, 0xA0FFC0, Deco::Sparkle };
    const Theme JULY4      = { "Independence",   0xFFFFFF, 0xFF3030, 0x4060FF, Deco::Confetti };
    const Theme HALLOWEEN  = { "Halloween",      0xFF7000, 0xA040FF, 0xFFB050, Deco::Sparkle };
    const Theme THANKS     = { "Thanksgiving",   0xFFA030, 0xC06020, 0xFFD080, Deco::None };
    const Theme CHRISTMAS  = { "Christmas",      0xFF3030, 0x30D040, 0xFFFFFF, Deco::Snow };
    const Theme NYE        = { "New Year's Eve", 0xFFD700, 0xFFFFFF, 0xC0C0FF, Deco::Confetti };

    // Anonymous Gregorian algorithm (Meeus/Jones/Butcher): month (1-12) and day of Easter Sunday
    void easter(int year, int& month, int& day) {
      int a = year % 19, b = year / 100, c = year % 100, d = b / 4, e = b % 4, f = (b + 8) / 25, g = (b - f + 1) / 3;
      int h = (19 * a + b - d - g + 15) % 30, i = c / 4, k = c % 4, l = (32 + 2 * e + 2 * i - h - k) % 7;
      int m = (a + 11 * h + 22 * l) / 451;
      month = (h + l - 7 * m + 114) / 31;
      day = ((h + l - 7 * m + 114) % 31) + 1;
    }
  }

  namespace { Theme custom = { "custom", 0xFFFFFF, 0xFFFFFF, 0xC0C0C0, Deco::Sparkle }; }

  struct tm sample(Sample s) {
    struct tm t = {};
    t.tm_year = 2026 - 1900; t.tm_mday = 1;
    switch (s) {
      case Sample::Christmas: t.tm_mon = 11; t.tm_mday = 25; break;
      case Sample::July4: t.tm_mon = 6; t.tm_mday = 4; break;
      case Sample::Valentine: t.tm_mon = 1; t.tm_mday = 14; break;
      case Sample::Halloween: t.tm_mon = 9; t.tm_mday = 31; break;
    }
    return t;
  }

  const Theme* forDate(const struct tm& lt) {
    const int mon = lt.tm_mon + 1, day = lt.tm_mday, year = lt.tm_year + 1900;
    // Your own dates win over the built-in list, so a birthday can override a holiday on the same day.
    for (uint8_t i = 0; i < g_cfg.display.nholidays && i < MAX_CUSTOM_HOLIDAYS; i++) {
      const HolidayConfig& h = g_cfg.display.holidays[i];
      if (h.month != mon || h.day != day) continue;
      custom.name = h.label[0] ? h.label : "custom";
      custom.time = h.color;
      custom.date = h.color;
      custom.text = 0xC0C0C0;
      custom.deco = Deco::Sparkle;
      return &custom;
    }
    if (mon == 1 && day == 1) return &NEW_YEAR;
    if (mon == 2 && day == 14) return &VALENTINE;
    if (mon == 3 && day == 17) return &ST_PATRICK;
    if (mon == 7 && day == 4) return &JULY4;
    if (mon == 10 && day == 31) return &HALLOWEEN;
    if (mon == 11 && lt.tm_wday == 4 && day >= 22 && day <= 28) return &THANKS;   // fourth Thursday
    if (mon == 12 && (day == 24 || day == 25)) return &CHRISTMAS;
    if (mon == 12 && day == 31) return &NYE;
    int em, ed;
    easter(year, em, ed);
    if (mon == em && day == ed) return &EASTER;
    return nullptr;
  }
}
