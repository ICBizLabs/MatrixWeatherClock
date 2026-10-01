// Edge cases for src/time/posix_tz.cpp that no real zone reaches: transitions dated near 1 January (which land in
// the neighbouring UTC year for far-eastern and far-western zones), malformed rules that must be rejected rather
// than overflowing, and instants past 2038 and before 1970. Built and run by tools/check_timezones.py.
#include "../src/time/posix_tz.h"
#include <cstdio>
#include <cstring>
int fails = 0;
void expect_off(const char* rule, long long utc, int want, const char* what) {
  posix_tz::Rule r;
  if (!posix_tz::parse(rule, r)) { printf("FAIL %-52s parse rejected %s\n", what, rule); fails++; return; }
  const int got = posix_tz::offset_at(r, utc);
  printf("%-4s %-52s want %+7d got %+7d\n", got == want ? "ok" : "FAIL", what, want, got);
  if (got != want) fails++;
}
void expect_reject(const char* rule, const char* what) {
  posix_tz::Rule r;
  const bool ok = posix_tz::parse(rule, r);
  printf("%-4s %-52s rejected=%s\n", ok ? "FAIL" : "ok", what, ok ? "no" : "yes");
  if (ok) fails++;
}
void expect_tm(const char* rule, long long utc, const char* want, const char* what) {
  struct tm t;
  if (!posix_tz::zone_tm(rule, utc, t)) { printf("FAIL %-52s zone_tm failed\n", what); fails++; return; }
  char buf[64];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d wday=%d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
           t.tm_hour, t.tm_min, t.tm_sec, t.tm_wday);
  printf("%-4s %-52s %s\n", strcmp(buf, want) ? "FAIL" : "ok", what, buf);
  if (strcmp(buf, want)) fails++;
}
void expect_isdst(const char* rule, long long utc, int want, const char* what) {
  struct tm t;
  if (!posix_tz::zone_tm(rule, utc, t)) { printf("FAIL %-52s zone_tm failed\n", what); fails++; return; }
  printf("%-4s %-52s tm_isdst want %d got %d\n", t.tm_isdst == want ? "ok" : "FAIL", what, want, t.tm_isdst);
  if (t.tm_isdst != want) fails++;
}
int main() {
  // codex finding 1: a transition dated near 1 January falls in the previous UTC year for a far-eastern zone
  expect_off("AAA-14BBB,M1.1.4/0,M2.1.0/0", 1767177000LL, 15 * 3600, "UTC+14, DST starts Thu 1 Jan 2026 local");
  expect_off("AAA-14BBB,M1.1.4/0,M2.1.0/0", 1767177000LL - 3600, 14 * 3600, "...and an hour before that change");
  // the mirror case: a far-western zone whose transition lands in the next UTC year
  expect_off("AAA11BBB,M12.5.0/23,M2.1.0/0", 1767225600LL, -10 * 3600, "UTC-11, DST starts late Dec 2025 local");

  // codex finding 2: malformed numbers must be rejected, not overflow on the way to the range check
  expect_reject("ABC999999999999999999999999999", "absurd offset");
  expect_reject("ABC0DEF,M999999999999999.1.0,M2.1.0", "absurd month");
  expect_reject("ABC0DEF,M3.2.0/999999999999,M2.1.0", "absurd transition hour");
  expect_reject("ABC0DEF,M3.2.0", "one rule only");
  expect_reject("ABC0DEF,M3.2.0,M11.1.0,junk", "trailing rubbish");
  expect_reject("AB5", "name too short");
  expect_reject("ABC0DEF,J100,J200", "Julian form, deliberately unsupported");
  expect_reject("", "empty");

  // codex finding 3: no time_t anywhere, so past 2038 is fine even where time_t is 32 bits
  expect_tm("UTC0", 2147483648LL, "2038-01-19 03:14:08 wday=2", "2038 rollover");
  expect_tm("UTC0", 4102444800LL, "2100-01-01 00:00:00 wday=5", "year 2100 (not a leap year)");
  expect_tm("EST5EDT,M3.2.0,M11.1.0", 1790653692LL, "2026-09-28 23:48:12 wday=1", "New York, DST");
  expect_tm("IST-5:30", 1790653692LL, "2026-09-29 09:18:12 wday=2", "India, half-hour offset");
  expect_tm("NPT-5:45", 1790653692LL, "2026-09-29 09:33:12 wday=2", "Nepal, quarter-hour offset");
  expect_tm("UTC0", -86401LL, "1969-12-30 23:59:59 wday=2", "before 1970 (floor division)");
  // tm_isdst must distinguish summer time, not sit at zero all year
  expect_isdst("EST5EDT,M3.2.0,M11.1.0", 1790653692LL, 1, "New York in September is on summer time");
  expect_isdst("EST5EDT,M3.2.0,M11.1.0", 1800000000LL, 0, "New York in January is not");
  expect_isdst("MST7", 1790653692LL, 0, "Arizona never is");
  expect_isdst("AEST-10AEDT,M10.1.0,M4.1.0/3", 1790653692LL, 0, "Sydney in September is not yet");
  expect_isdst("AEST-10AEDT,M10.1.0,M4.1.0/3", 1800000000LL, 1, "Sydney in January is");
  printf("\n%s\n", fails ? "FAILURES" : "all edge cases pass");
  return fails ? 1 : 0;
}
