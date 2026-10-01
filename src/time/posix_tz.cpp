#include "posix_tz.h"
#include <ctype.h>

namespace posix_tz {
  namespace {
    // Howard Hinnant's civil-date algorithms, kept local so this file builds on its own for the host test and so
    // nothing here depends on time_t, which is only 32 bits on some builds.
    int64_t days_from_civil(int y, unsigned m, unsigned d) {
      y -= m <= 2;
      const int64_t era = (y >= 0 ? y : y - 399) / 400;
      const unsigned yoe = (unsigned)(y - era * 400);
      const unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2u) / 5u + d - 1;
      const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
      return era * 146097 + (int64_t)doe - 719468;
    }
    void civil_from_days(int64_t z, int& y, unsigned& m, unsigned& d) {
      z += 719468;
      const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
      const unsigned doe = (unsigned)(z - era * 146097);
      const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
      const int64_t yy = (int64_t)yoe + era * 400;
      const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
      const unsigned mp = (5 * doy + 2) / 153;
      d = doy - (153 * mp + 2) / 5 + 1;
      m = mp + (mp < 10 ? 3 : -9);
      y = (int)(yy + (m <= 2));
    }
    // Floor division, so instants before 1970 still land on the right day.
    int64_t floor_days(int64_t secs, int64_t& rem) {
      int64_t days = secs / 86400;
      rem = secs % 86400;
      if (rem < 0) { rem += 86400; days--; }
      return days;
    }
    int days_in_month(int y, unsigned m) {
      static const int N[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
      if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
      return N[m - 1];
    }

    bool skipName(const char*& p) {
      if (*p == '<') {                       // <+03> style names, which this firmware's own table avoids
        p++;
        while (*p && *p != '>') p++;
        if (*p != '>') return false;
        p++;
        return true;
      }
      int n = 0;
      while (isalpha((unsigned char)*p)) { p++; n++; }
      return n >= 3;                         // POSIX asks for at least three characters
    }
    // Bounded so that malformed input is rejected rather than overflowing on the way to the range check.
    bool readInt(const char*& p, int& out, int cap) {
      if (!isdigit((unsigned char)*p)) return false;
      int v = 0, digits = 0;
      while (isdigit((unsigned char)*p)) {
        if (++digits > 9) return false;      // nothing legitimate is that long, and nine digits cannot overflow
        v = v * 10 + (*p++ - '0');
      }
      if (v > cap) return false;
      out = v;
      return true;
    }
    // [+-]h[h][:mm[:ss]]; hmax is 24 for a zone offset, 167 for a transition time
    bool readTime(const char*& p, int32_t& secs, int hmax) {
      int sign = 1;
      if (*p == '+') p++;
      else if (*p == '-') { sign = -1; p++; }
      int h = 0, m = 0, s = 0;
      if (!readInt(p, h, hmax)) return false;
      if (*p == ':') {
        p++;
        if (!readInt(p, m, 59)) return false;
        if (*p == ':') {
          p++;
          if (!readInt(p, s, 59)) return false;
        }
      }
      secs = sign * (h * 3600 + m * 60 + s);
      return true;
    }
    bool readWhen(const char*& p, When& w) {
      if (*p != 'M') return false;           // Jn and n are legal POSIX but nothing here uses them
      p++;
      int mon = 0, week = 0, dow = 0;
      if (!readInt(p, mon, 12) || mon < 1) return false;
      if (*p++ != '.') return false;
      if (!readInt(p, week, 5) || week < 1) return false;
      if (*p++ != '.') return false;
      if (!readInt(p, dow, 6)) return false;
      w.mon = (uint8_t)mon; w.week = (uint8_t)week; w.dow = (uint8_t)dow;
      w.sec = 2 * 3600;                      // POSIX default when no time is given
      if (*p == '/') { p++; if (!readTime(p, w.sec, 167)) return false; }
      return true;
    }

    // The UTC instant of one transition in local calendar year y. POSIX states the time in whichever offset is in
    // effect just before the change, which is why the caller passes that in.
    int64_t whenUtc(const When& w, int y, int32_t off_before) {
      const int64_t first = days_from_civil(y, w.mon, 1);
      const int wd_first = (int)(((first % 7) + 7 + 4) % 7);          // 1970-01-01 was a Thursday
      int day = 1 + ((int)w.dow - wd_first + 7) % 7;                  // the first such weekday in the month
      day += 7 * (w.week - 1);
      while (day > days_in_month(y, w.mon)) day -= 7;                 // week 5 means "the last one"
      return days_from_civil(y, w.mon, (unsigned)day) * 86400 + w.sec - off_before;
    }
  }

  bool parse(const char* s, Rule& out) {
    if (!s || !*s) return false;
    const char* p = s;
    out = Rule();
    if (!skipName(p)) return false;
    if (!readTime(p, out.std_off, 24)) return false;
    out.std_off = -out.std_off;                    // POSIX counts west as positive; we want east
    out.has_dst = false;
    out.dst_off = out.std_off;
    if (!*p) return true;                          // standard time all year, e.g. "MST7"
    if (!skipName(p)) return false;                // the daylight name
    if (*p && *p != ',') {
      int32_t d;
      if (!readTime(p, d, 24)) return false;
      out.dst_off = -d;
    } else {
      out.dst_off = out.std_off + 3600;            // the default saving is one hour
    }
    if (*p != ',') return false;                   // a daylight name with no rules tells us nothing
    p++;
    if (!readWhen(p, out.start)) return false;
    if (*p++ != ',') return false;
    if (!readWhen(p, out.end)) return false;
    if (*p) return false;                          // trailing rubbish means we misread something
    out.has_dst = true;
    return true;
  }

  int32_t offset_at(const Rule& r, int64_t utc) {
    if (!r.has_dst) return r.std_off;
    int64_t rem = 0;
    int y = 0; unsigned mo = 0, dd = 0;
    civil_from_days(floor_days(utc, rem), y, mo, dd);
    // A rule's dates are local ones, so a transition dated near 1 January can land in the neighbouring UTC year for
    // a far-eastern or far-western zone. Looking at three years and taking the most recent change at or before this
    // instant covers that, and makes the southern hemisphere (where the year wraps) fall out for free.
    // Four years rather than three: a transition time may be up to 167 hours, so a rule dated in late December can
    // land over a week away and the most recent change before an early-January instant can belong to two years back.
    // Nothing in the zone table does that, but the scan costs eight comparisons and the caveat is then gone.
    int32_t off = r.std_off;
    int64_t latest = INT64_MIN;
    for (int yy = y - 2; yy <= y + 1; yy++) {
      const int64_t s = whenUtc(r.start, yy, r.std_off);
      const int64_t e = whenUtc(r.end, yy, r.dst_off);
      if (s <= utc && s > latest) { latest = s; off = r.dst_off; }
      if (e <= utc && e > latest) { latest = e; off = r.std_off; }
    }
    return off;
  }

  bool zone_tm(const char* posix, int64_t utc, struct tm& out) {
    Rule r;
    if (!parse(posix, r)) return false;
    const int32_t off = offset_at(r, utc);
    const int64_t local = utc + off;
    int64_t rem = 0;
    const int64_t days = floor_days(local, rem);
    int y = 0; unsigned mo = 0, dd = 0;
    civil_from_days(days, y, mo, dd);
    out = tm();
    out.tm_year = y - 1900;
    out.tm_mon = (int)mo - 1;
    out.tm_mday = (int)dd;
    out.tm_hour = (int)(rem / 3600);
    out.tm_min = (int)(rem % 3600 / 60);
    out.tm_sec = (int)(rem % 60);
    out.tm_wday = (int)(((days % 7) + 11) % 7);    // 1970-01-01 was a Thursday
    out.tm_yday = (int)(days - days_from_civil(y, 1, 1));
    out.tm_isdst = (r.has_dst && off != r.std_off) ? 1 : 0;   // so callers can tell summer time apart
    return true;
  }
}
