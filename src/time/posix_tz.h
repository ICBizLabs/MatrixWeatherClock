#pragma once
#include <stdint.h>
#include <time.h>

// A self-contained evaluator for POSIX TZ rules such as "EST5EDT,M3.2.0,M11.1.0".
//
// The C library can already do this, but only through the process-global TZ: setenv plus tzset on every lookup. That
// needs a lock, and on this newlib each pair leaks about 50 bytes -- with four zones refreshed twice a second that is
// 24 KB a minute, which exhausts the internal heap and panics the device in about four minutes. Working the rule out
// ourselves is plain arithmetic: no allocation, no global state, no lock, and no effect on the local timezone.
//
// Handles the forms the zone table uses: STD[offset], STD[offset]DST[offset], and the ",Mm.w.d[/time]" transition
// pair, including hours past 24 (Israel switches at 26:00) and quarter-hour offsets. The Julian-day forms (Jn and n)
// are rejected rather than guessed at.
//
// The model is "the most recent transition at or before this instant", searched over four calendar years. The C
// library instead decides within a single year, the one the instant falls in by UTC, and treats a start later than
// its end as the southern hemisphere. The two agree for every zone in tz_table.cpp -- tools/check_timezones.py
// checks 844,464 instants against the IANA database -- and differ only for hand-written rules whose transitions
// spill across a year boundary, where this one is the more literal reading. It needs no hemisphere special case.
namespace posix_tz {
  struct When {
    uint8_t mon;      // 1..12
    uint8_t week;     // 1..5, where 5 means the last such weekday in the month
    uint8_t dow;      // 0 = Sunday
    int32_t sec;      // seconds into that day, in the time in effect just before the change; may exceed one day
  };
  struct Rule {
    int32_t std_off;  // seconds EAST of UTC (POSIX writes these west-positive; already flipped here)
    int32_t dst_off;
    bool has_dst;
    When start, end;
  };

  bool parse(const char* s, Rule& out);                          // false when the form is not one we handle;
                                                                 // out may be half-written, so ignore it on false
  int32_t offset_at(const Rule& r, int64_t utc);                 // seconds east of UTC at that instant
  // Local time in that zone. Fills every field of out, tm_isdst included. Deliberately does not go through time_t,
  // so it is right past 2038 even where time_t is 32 bits.
  bool zone_tm(const char* posix, int64_t utc, struct tm& out);
}
