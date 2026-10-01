#pragma once
#include <stddef.h>

// Time zones offered in the web UI. There is no IANA database on the device, so each zone carries a POSIX rule:
// the main clock hands its own to the C library (setenv TZ), and the world clock evaluates the others with
// time/posix_tz.h. Every rule here is checked against the real IANA database by tools/check_timezones.py, which
// parses tz_table.cpp -- run it after touching the table. A wrong daylight-saving rule looks perfect for months and
// then goes an hour out.
struct TzEntry { const char* id; const char* label; const char* posix; };

extern const TzEntry TZ_TABLE[];
extern const size_t TZ_TABLE_LEN;
// The first TZ_COMMON_LEN entries are UTC and the US zones: the short list the first-boot wizard offers, and all
// that /api/config carries. The whole table is served separately by GET /api/timezones, because 78 zones is far too
// much to put in a document that every page load fetches and the settings-restore path posts back whole.
extern const size_t TZ_COMMON_LEN;

const char* tz_posix_for(const char* id);   // nullptr when the id is not in the table
const char* tz_label_for(const char* id);   // nullptr when the id is not in the table
