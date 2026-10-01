#include "tz_table.h"
#include <string.h>

// UTC first, then the US and Canada, then west to east. See tz_table.h: tools/check_timezones.py verifies every
// rule below against the IANA database and must keep passing.
const TzEntry TZ_TABLE[] = {
  { "Etc/UTC",                      "UTC",              "UTC0" },

  { "America/New_York",             "Eastern",          "EST5EDT,M3.2.0,M11.1.0" },
  { "America/Chicago",              "Central",          "CST6CDT,M3.2.0,M11.1.0" },
  { "America/Denver",               "Mountain",         "MST7MDT,M3.2.0,M11.1.0" },
  { "America/Phoenix",              "Arizona",          "MST7" },
  { "America/Los_Angeles",          "Pacific",          "PST8PDT,M3.2.0,M11.1.0" },
  { "America/Anchorage",            "Alaska",           "AKST9AKDT,M3.2.0,M11.1.0" },
  { "Pacific/Honolulu",             "Hawaii",           "HST10" },
  { "America/Puerto_Rico",          "Puerto Rico",      "AST4" },
  { "Pacific/Guam",                 "Guam",             "ChST-10" },
  { "Pacific/Pago_Pago",            "Samoa",            "SST11" },

  { "America/Toronto",              "Toronto",          "EST5EDT,M3.2.0,M11.1.0" },
  { "America/Winnipeg",             "Winnipeg",         "CST6CDT,M3.2.0,M11.1.0" },
  { "America/Edmonton",             "Edmonton",         "CST6" },              // Alberta: permanent, no DST
  { "America/Vancouver",            "Vancouver",        "MST7" },              // British Columbia: permanent, no DST
  { "America/Halifax",              "Halifax",          "AST4ADT,M3.2.0,M11.1.0" },
  { "America/St_Johns",             "St John's",        "NST3:30NDT,M3.2.0,M11.1.0" },

  { "America/Mexico_City",          "Mexico City",      "CST6" },
  { "America/Bogota",               "Bogota",           "COT5" },
  { "America/Lima",                 "Lima",             "PET5" },
  { "America/Caracas",              "Caracas",          "VET4" },
  { "America/Santiago",             "Santiago",         "CLT4CLST,M9.1.6/24,M4.1.6/24" },
  { "America/Sao_Paulo",            "Sao Paulo",        "BRT3" },
  { "America/Argentina/Buenos_Aires", "Buenos Aires",   "ART3" },

  { "Atlantic/Reykjavik",           "Reykjavik",        "GMT0" },
  { "Europe/London",                "London",           "GMT0BST,M3.5.0/1,M10.5.0/2" },
  { "Europe/Dublin",                "Dublin",           "GMT0IST,M3.5.0/1,M10.5.0/2" },
  { "Europe/Lisbon",                "Lisbon",           "WET0WEST,M3.5.0/1,M10.5.0/2" },
  { "Europe/Madrid",                "Madrid",           "CET-1CEST,M3.5.0,M10.5.0/3" },
  { "Europe/Paris",                 "Paris",            "CET-1CEST,M3.5.0,M10.5.0/3" },
  { "Europe/Brussels",              "Brussels",         "CET-1CEST,M3.5.0,M10.5.0/3" },
  { "Europe/Amsterdam",             "Amsterdam",        "CET-1CEST,M3.5.0,M10.5.0/3" },
  { "Europe/Berlin",                "Berlin",           "CET-1CEST,M3.5.0,M10.5.0/3" },
  { "Europe/Zurich",                "Zurich",           "CET-1CEST,M3.5.0,M10.5.0/3" },
  { "Europe/Rome",                  "Rome",             "CET-1CEST,M3.5.0,M10.5.0/3" },
  { "Europe/Vienna",                "Vienna",           "CET-1CEST,M3.5.0,M10.5.0/3" },
  { "Europe/Prague",                "Prague",           "CET-1CEST,M3.5.0,M10.5.0/3" },
  { "Europe/Warsaw",                "Warsaw",           "CET-1CEST,M3.5.0,M10.5.0/3" },
  { "Europe/Stockholm",             "Stockholm",        "CET-1CEST,M3.5.0,M10.5.0/3" },
  { "Europe/Oslo",                  "Oslo",             "CET-1CEST,M3.5.0,M10.5.0/3" },
  { "Europe/Copenhagen",            "Copenhagen",       "CET-1CEST,M3.5.0,M10.5.0/3" },
  { "Europe/Athens",                "Athens",           "EET-2EEST,M3.5.0/3,M10.5.0/4" },
  { "Europe/Helsinki",              "Helsinki",         "EET-2EEST,M3.5.0/3,M10.5.0/4" },
  { "Europe/Bucharest",             "Bucharest",        "EET-2EEST,M3.5.0/3,M10.5.0/4" },
  { "Europe/Kyiv",                  "Kyiv",             "EET-2EEST,M3.5.0/3,M10.5.0/4" },
  { "Europe/Istanbul",              "Istanbul",         "TRT-3" },
  { "Europe/Moscow",                "Moscow",           "MSK-3" },

  { "Africa/Lagos",                 "Lagos",            "WAT-1" },
  { "Africa/Cairo",                 "Cairo",            "EET-2EEST,M4.5.5/0,M10.5.4/24" },
  { "Africa/Johannesburg",          "Johannesburg",     "SAST-2" },
  { "Africa/Nairobi",               "Nairobi",          "EAT-3" },
  { "Asia/Jerusalem",               "Jerusalem",        "IST-2IDT,M3.4.4/26,M10.5.0" },
  { "Asia/Riyadh",                  "Riyadh",           "AST-3" },
  { "Asia/Dubai",                   "Dubai",            "GST-4" },
  { "Asia/Tehran",                  "Tehran",           "IRST-3:30" },

  { "Asia/Karachi",                 "Karachi",          "PKT-5" },
  { "Asia/Kolkata",                 "India",            "IST-5:30" },
  { "Asia/Kathmandu",               "Kathmandu",        "NPT-5:45" },
  { "Asia/Dhaka",                   "Dhaka",            "BST-6" },
  { "Asia/Bangkok",                 "Bangkok",          "ICT-7" },
  { "Asia/Jakarta",                 "Jakarta",          "WIB-7" },
  { "Asia/Singapore",               "Singapore",        "SGT-8" },
  { "Asia/Hong_Kong",               "Hong Kong",        "HKT-8" },
  { "Asia/Shanghai",                "Shanghai",         "CST-8" },
  { "Asia/Taipei",                  "Taipei",           "CST-8" },
  { "Asia/Manila",                  "Manila",           "PHT-8" },
  { "Asia/Seoul",                   "Seoul",            "KST-9" },
  { "Asia/Tokyo",                   "Tokyo",            "JST-9" },

  { "Australia/Perth",              "Perth",            "AWST-8" },
  { "Australia/Darwin",             "Darwin",           "ACST-9:30" },
  { "Australia/Adelaide",           "Adelaide",         "ACST-9:30ACDT,M10.1.0,M4.1.0/3" },
  { "Australia/Brisbane",           "Brisbane",         "AEST-10" },
  { "Australia/Sydney",             "Sydney",           "AEST-10AEDT,M10.1.0,M4.1.0/3" },
  { "Australia/Melbourne",          "Melbourne",        "AEST-10AEDT,M10.1.0,M4.1.0/3" },
  { "Australia/Hobart",             "Hobart",           "AEST-10AEDT,M10.1.0,M4.1.0/3" },
  { "Pacific/Auckland",             "Auckland",         "NZST-12NZDT,M9.5.0,M4.1.0/3" },
  { "Pacific/Fiji",                 "Fiji",             "FJT-12" },
  { "Pacific/Kiritimati",           "Kiritimati",       "LINT-14" },
};
const size_t TZ_TABLE_LEN = sizeof(TZ_TABLE) / sizeof(TZ_TABLE[0]);
const size_t TZ_COMMON_LEN = 11;   // UTC plus the ten US zones above; tools/check_timezones.py checks this

const char* tz_posix_for(const char* id) {
  if (!id || !*id) return nullptr;
  for (size_t i = 0; i < TZ_TABLE_LEN; i++) if (strcmp(TZ_TABLE[i].id, id) == 0) return TZ_TABLE[i].posix;
  return nullptr;
}

const char* tz_label_for(const char* id) {
  if (!id || !*id) return nullptr;
  for (size_t i = 0; i < TZ_TABLE_LEN; i++) if (strcmp(TZ_TABLE[i].id, id) == 0) return TZ_TABLE[i].label;
  return nullptr;
}
