// Host harness for src/time/posix_tz.cpp, used by tools/check_timezones.py.
// Reads "<utc seconds> <POSIX rule>" lines on stdin and prints the offset east of UTC the firmware would compute.
#include "../src/time/posix_tz.h"
#include <cstdio>

int main() {
  char line[256];
  while (fgets(line, sizeof(line), stdin)) {
    long long utc = 0;
    char rule[176] = "";
    if (sscanf(line, "%lld %175[^\n]", &utc, rule) != 2) { printf("BAD_LINE\n"); continue; }
    posix_tz::Rule r;
    if (!posix_tz::parse(rule, r)) { printf("PARSE_FAIL\n"); continue; }
    printf("%d\n", (int)posix_tz::offset_at(r, utc));
  }
  return 0;
}
