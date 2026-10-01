// AppConfig <-> JSON in both directions, with server-side validation. One place for every key name.
#include "config.h"
#include "io/actions.h"
#include "display/themes.h"
#include "util/timeutil.h"
#include <strings.h>

namespace {
  const char* const SEVERITY_NAMES[] = { "Unknown", "Minor", "Moderate", "Severe", "Extreme" };
  const char* const PAGE_NAMES[] = { "date", "temp", "cond", "wind", "hilo", "feels", "sun", "indoor", "air", "baro", "tide", "moon",
                                     "uv", "sky", "rain", "water", "world", "event" };
  // Shown in the web UI's page picker; index-aligned with PAGE_NAMES and PageId.
  const char* const PAGE_LABELS[] = { "Date", "Temperature", "Conditions", "Wind", "High / low", "Feels like",
                                      "Sunrise / sunset", "Indoor temperature and humidity", "Air quality",
                                      "Barometer", "Tide", "Moon phase", "Ultraviolet index",
                                      "Cloud and visibility", "Rainfall", "Sea temperature", "World clock",
                                      "Countdown" };
  const char* const CHIME_NAMES[] = { "none", "two_tone", "triple_beep", "chirp", "eas_attention", "eas_full", "nws_1050",
                                      "siren_wail", "siren_yelp", "siren_hilo", "alarm_beeps", "doorbell", "sos", "arpeggio", "sonar" };
  constexpr uint8_t CHIME_COUNT = (uint8_t)ChimeStyle::COUNT;
  const char* const DRIVER_NAMES[] = { "SHIFTREG", "FM6124", "FM6126A", "ICN2038S", "MBI5124", "DP3246" };

  bool nameLookup(const char* const* names, size_t n, const char* s, uint8_t& out) {
    if (!s) return false;
    for (size_t i = 0; i < n; i++) if (strcasecmp(names[i], s) == 0) { out = (uint8_t)i; return true; }
    return false;
  }

  // ---- readers: each returns false only on a validation error; sets touched when the key exists ----
  template <size_t N>
  bool getStr(JsonObjectConst o, const char* k, char (&dst)[N], bool& touched, String& err, bool allowEmpty = true) {
    JsonVariantConst v = o[k];
    if (v.isNull()) return true;
    if (!v.is<const char*>()) { err = String(k) + ": must be a string"; return false; }
    const char* s = v.as<const char*>();
    if (!allowEmpty && s[0] == '\0') { err = String(k) + ": must not be empty"; return false; }
    if (strlen(s) >= N) { err = String(k) + ": too long"; return false; }
    strlcpy(dst, s, N);
    touched = true;
    return true;
  }
  bool getBool(JsonObjectConst o, const char* k, bool& dst, bool& touched, String& err) {
    JsonVariantConst v = o[k];
    if (v.isNull()) return true;
    if (!v.is<bool>()) { err = String(k) + ": must be true/false"; return false; }
    dst = v.as<bool>();
    touched = true;
    return true;
  }
  template <typename T>
  bool getNum(JsonObjectConst o, const char* k, T& dst, bool& touched, String& err, double lo, double hi) {
    JsonVariantConst v = o[k];
    if (v.isNull()) return true;
    double d;
    if (v.is<double>() || v.is<long>()) d = v.as<double>();
    else if (v.is<const char*>()) {                       // accept "34" as well as 34 (HTML selects send strings)
      const char* str = v.as<const char*>();
      char* end = nullptr;
      d = strtod(str, &end);
      if (end == str || *end != '\0') { err = String(k) + ": must be a number"; return false; }
    } else { err = String(k) + ": must be a number"; return false; }
    if (d < lo || d > hi) { err = String(k) + ": out of range"; return false; }
    dst = (T)d;
    touched = true;
    return true;
  }
  bool getHHMM(JsonObjectConst o, const char* k, uint16_t& dst, bool& touched, String& err) {
    JsonVariantConst v = o[k];
    if (v.isNull()) return true;
    int m = v.is<const char*>() ? hhmm_parse(v.as<const char*>()) : -1;
    if (m < 0) { err = String(k) + ": expected HH:MM"; return false; }
    dst = (uint16_t)m;
    touched = true;
    return true;
  }
  bool getColor(JsonObjectConst o, const char* k, uint32_t& dst, bool& touched, String& err) {
    JsonVariantConst v = o[k];
    if (v.isNull()) return true;
    const char* s = v.as<const char*>();
    if (!s || s[0] != '#' || strlen(s) != 7) { err = String(k) + ": expected #RRGGBB"; return false; }
    char* end = nullptr;
    unsigned long val = strtoul(s + 1, &end, 16);
    if (!end || *end != '\0') { err = String(k) + ": expected #RRGGBB"; return false; }
    dst = (uint32_t)val;
    touched = true;
    return true;
  }
  bool getSeverity(JsonObjectConst o, const char* k, Severity& dst, bool& touched, String& err) {
    JsonVariantConst v = o[k];
    if (v.isNull()) return true;
    Severity s;
    if (!severity_parse(v.as<const char*>(), s)) { err = String(k) + ": unknown severity"; return false; }
    dst = s;
    touched = true;
    return true;
  }

  void colorToHex(uint32_t c, char* out, size_t n) { snprintf(out, n, "#%06lX", (unsigned long)(c & 0xFFFFFF)); }
  void putHHMM(JsonObject o, const char* k, uint16_t m) { char b[8]; hhmm_format(m, b, sizeof(b)); o[k] = b; }
  void putColor(JsonObject o, const char* k, uint32_t c) { char b[10]; colorToHex(c, b, sizeof(b)); o[k] = b; }

  bool validHostname(const char* h) {
    size_t n = strlen(h);
    if (n == 0 || n > 31) return false;
    for (size_t i = 0; i < n; i++) {
      char c = h[i];
      if (!(isalnum((unsigned char)c) || c == '-')) return false;
      if (isupper((unsigned char)c)) return false;
    }
    return h[0] != '-' && h[n - 1] != '-';
  }

  bool validIp(const char* s) {            // dotted quad, four decimal octets 0..255
    int parts = 0;
    for (const char* p = s; ; ) {
      if (!isdigit((unsigned char)*p)) return false;
      int v = 0, digits = 0;
      while (isdigit((unsigned char)*p)) { v = v * 10 + (*p++ - '0'); if (++digits > 3) return false; }
      if (v > 255) return false;
      parts++;
      if (*p == '\0') break;
      if (*p != '.' || parts == 4) return false;
      p++;
    }
    return parts == 4;
  }
}

const char* severity_name(Severity s) { uint8_t i = (uint8_t)s; return i < 5 ? SEVERITY_NAMES[i] : "Unknown"; }
bool severity_parse(const char* s, Severity& out) { uint8_t i; if (!nameLookup(SEVERITY_NAMES, 5, s, i)) return false; out = (Severity)i; return true; }
const char* page_name(uint8_t id) { return id < PAGE_COUNT ? PAGE_NAMES[id] : "?"; }
const char* page_label(uint8_t id) { return id < PAGE_COUNT ? PAGE_LABELS[id] : "?"; }
bool page_parse(const char* s, uint8_t& out) { return nameLookup(PAGE_NAMES, PAGE_COUNT, s, out); }
const char* chime_name(ChimeStyle c) { uint8_t i = (uint8_t)c; return i < CHIME_COUNT ? CHIME_NAMES[i] : "none"; }
bool chime_parse(const char* s, ChimeStyle& out) { uint8_t i; if (!nameLookup(CHIME_NAMES, CHIME_COUNT, s, i)) return false; out = (ChimeStyle)i; return true; }
const char* panel_driver_name(uint8_t d) { return d < PANEL_DRIVER_COUNT ? DRIVER_NAMES[d] : DRIVER_NAMES[0]; }
bool panel_driver_parse(const char* s, uint8_t& out) { return nameLookup(DRIVER_NAMES, PANEL_DRIVER_COUNT, s, out); }

bool config_from_json(JsonObjectConst src, AppConfig& c, uint32_t& changed, String& err) {
  if (src.isNull()) { err = "expected a JSON object"; return false; }
  bool t;

  JsonObjectConst o = src["wifi"];
  if (!o.isNull()) {
    t = false;
    if (!getStr(o, "ssid", c.wifi.ssid, t, err)) return false;
    JsonVariantConst p = o["pass"];
    if (p.is<const char*>() && strcmp(p.as<const char*>(), "***") != 0) {   // "***" keeps the stored secret
      if (!getStr(o, "pass", c.wifi.pass, t, err)) return false;
    }
    JsonVariantConst ap = o["ap_pass"];
    if (ap.is<const char*>() && strcmp(ap.as<const char*>(), "***") != 0) {
      if (!getStr(o, "ap_pass", c.wifi.ap_pass, t, err)) return false;
      if (c.wifi.ap_pass[0] && strlen(c.wifi.ap_pass) < 8) { err = "ap_pass: at least 8 characters"; return false; }
    }
    if (!getStr(o, "hostname", c.wifi.hostname, t, err)) return false;
    if (!strcmp(c.wifi.hostname, "matrixclock")) { strlcpy(c.wifi.hostname, "matrixweatherclock", sizeof(c.wifi.hostname)); t = true; }   // project renamed in 0.10.0
    if (!validHostname(c.wifi.hostname)) { err = "hostname: use 1-31 lowercase letters, digits or '-'"; return false; }
    if (!getNum(o, "tx_power", c.wifi.tx_power, t, err, 8, 84)) return false;
    if (!getStr(o, "ssid2", c.wifi.ssid2, t, err)) return false;
    JsonVariantConst p2 = o["pass2"];
    if (p2.is<const char*>() && strcmp(p2.as<const char*>(), "***") != 0) {
      if (!getStr(o, "pass2", c.wifi.pass2, t, err)) return false;
    }
    if (!getBool(o, "dhcp", c.wifi.dhcp, t, err)) return false;
    if (!getStr(o, "ip", c.wifi.ip, t, err)) return false;
    if (!getStr(o, "gateway", c.wifi.gateway, t, err)) return false;
    if (!getStr(o, "netmask", c.wifi.netmask, t, err)) return false;
    if (!getStr(o, "dns", c.wifi.dns, t, err)) return false;
    for (const char* f : { c.wifi.ip, c.wifi.gateway, c.wifi.netmask, c.wifi.dns }) {
      if (f[0] && !validIp(f)) { err = "wifi: ip, gateway, netmask and dns must be dotted quads"; return false; }
    }
    if (!c.wifi.dhcp && (!c.wifi.ip[0] || !c.wifi.gateway[0])) { err = "wifi: a static address needs ip and gateway"; return false; }
    if (t) changed |= CHG_WIFI;
  }

  o = src["location"];
  if (!o.isNull()) {
    t = false;
    if (!getNum(o, "lat", c.location.lat, t, err, -90, 90)) return false;
    if (!getNum(o, "lon", c.location.lon, t, err, -180, 180)) return false;
    if (!getStr(o, "name", c.location.name, t, err)) return false;
    if (t) changed |= CHG_LOCATION;
  }

  o = src["time"];
  if (!o.isNull()) {
    t = false;
    if (!getStr(o, "tz_id", c.time.tz_id, t, err)) return false;
    if (!getStr(o, "tz_posix", c.time.tz_posix, t, err, false)) return false;
    if (!getStr(o, "ntp1", c.time.ntp1, t, err, false)) return false;
    if (!getStr(o, "ntp2", c.time.ntp2, t, err)) return false;
    if (!getBool(o, "use_24h", c.time.use_24h, t, err)) return false;
    if (!getBool(o, "show_seconds", c.time.show_seconds, t, err)) return false;
    JsonVariantConst dord = o["date_order"];
    if (!dord.isNull()) {
      const char* v = dord | "mdy";
      if (!strcasecmp(v, "mdy")) c.time.date_order = 0;
      else if (!strcasecmp(v, "dmy")) c.time.date_order = 1;
      else if (!strcasecmp(v, "iso")) c.time.date_order = 2;
      else { err = "time.date_order: mdy, dmy or iso"; return false; }
      t = true;
    }
    // Legacy: the one "second time zone" became the first row of the world clock in 0.16.0. These keys are still
    // accepted so an existing config.json keeps working, and are no longer written, so it upgrades on the first save.
    {
      char lid[40] = "", lposix[64] = "", llabel[12] = "";
      bool lt = false;
      if (!getStr(o, "tz2_id", lid, lt, err)) return false;
      if (!getStr(o, "tz2_posix", lposix, lt, err)) return false;
      if (!getStr(o, "tz2_label", llabel, lt, err)) return false;
      WorldZone& z0 = c.world.zones[0];
      if (lt && z0.id[0] == '\0' && z0.posix[0] == '\0') {
        strlcpy(z0.id, lid, sizeof(z0.id));
        strlcpy(z0.posix, lposix, sizeof(z0.posix));
        strlcpy(z0.label, llabel, sizeof(z0.label));
        changed |= CHG_WORLD;
      }
    }
    if (t) changed |= CHG_TIME;
  }

  o = src["weather"];
  if (!o.isNull()) {
    t = false;
    if (!getBool(o, "enabled", c.weather.enabled, t, err)) return false;
    JsonVariantConst u = o["units"];
    if (!u.isNull()) {
      const char* s = u.as<const char*>();
      if (s && strcasecmp(s, "imperial") == 0) c.weather.imperial = true;
      else if (s && strcasecmp(s, "metric") == 0) c.weather.imperial = false;
      else { err = "units: imperial or metric"; return false; }
      t = true;
    }
    if (!getNum(o, "refresh_min", c.weather.refresh_min, t, err, 5, 1440)) return false;
    if (!getNum(o, "forecast_days", c.weather.forecast_days, t, err, 1, 3)) return false;
    if (t) changed |= CHG_WEATHER;
  }

  o = src["alerts"];
  if (!o.isNull()) {
    t = false;
    if (!getBool(o, "enabled", c.alerts.enabled, t, err)) return false;
    if (!getNum(o, "poll_sec", c.alerts.poll_sec, t, err, 60, 3600)) return false;
    if (!getStr(o, "user_agent_contact", c.alerts.user_agent_contact, t, err)) return false;
    if (!getSeverity(o, "min_severity", c.alerts.min_severity, t, err)) return false;
    JsonVariantConst ig = o["ignored_events"];
    if (!ig.isNull()) {
      String joined;
      if (ig.is<JsonArrayConst>()) {
        for (JsonVariantConst v : ig.as<JsonArrayConst>()) {
          const char* s = v.as<const char*>();
          if (!s || !*s) continue;
          if (joined.length()) joined += ',';
          joined += s;
        }
      } else if (ig.is<const char*>()) joined = ig.as<const char*>();
      else { err = "ignored_events: expected an array of strings"; return false; }
      if (joined.length() >= sizeof(c.alerts.ignored_events)) { err = "ignored_events: too long"; return false; }
      strlcpy(c.alerts.ignored_events, joined.c_str(), sizeof(c.alerts.ignored_events));
      t = true;
    }
    if (!getSeverity(o, "chime_min_severity", c.alerts.chime_min_severity, t, err)) return false;
    if (!getNum(o, "flash_frame_sec", c.alerts.flash_frame_sec, t, err, 0, 3600)) return false;
    if (!getNum(o, "banner_px_per_s", c.alerts.banner_px_per_s, t, err, 5, 80)) return false;
    if (!getBool(o, "tls_verify", c.alerts.tls_verify, t, err)) return false;
    if (t) changed |= CHG_ALERTS;
  }

  o = src["display"];
  if (!o.isNull()) {
    t = false;
    DisplayConfig& d = c.display;
    if (!getNum(o, "brightness", d.brightness, t, err, 1, 255)) return false;
    if (!getNum(o, "gamma", d.gamma, t, err, 1.0, 3.0)) return false;
    if (!getNum(o, "page_sec", d.page_sec, t, err, 2, 60)) return false;
    JsonVariantConst pg = o["pages"];
    if (!pg.isNull()) {
      if (!pg.is<JsonArrayConst>()) { err = "pages: expected an array"; return false; }
      uint8_t n = 0;
      for (JsonVariantConst v : pg.as<JsonArrayConst>()) {
        uint8_t id;
        if (!page_parse(v.as<const char*>(), id)) { err = "pages: unknown page name"; return false; }
        if (n < PAGE_COUNT) d.pages[n++] = id;
      }
      if (n == 0) { err = "pages: at least one page"; return false; }
      d.npages = n;
      t = true;
    }
    if (!getBool(o, "forecast_page", d.forecast_page, t, err)) return false;
    if (!getNum(o, "forecast_every_n_cycles", d.forecast_every_n_cycles, t, err, 1, 20)) return false;
    if (!getBool(o, "colon_blink", d.colon_blink, t, err)) return false;
    if (!getNum(o, "ip_on_connect_sec", d.ip_on_connect_sec, t, err, 0, 120)) return false;
    if (!getBool(o, "hourly_page", d.hourly_page, t, err)) return false;
    if (!getBool(o, "transitions", d.transitions, t, err)) return false;
    if (!getBool(o, "precip_fx", d.precip_fx, t, err)) return false;
    if (!getBool(o, "holiday_themes", d.holiday_themes, t, err)) return false;
    if (!getNum(o, "holidays_enabled", d.holidays_enabled, t, err, 0, 4294967295.0)) return false;
    // An id from themes::HOLIDAYS or themes::PALETTES, or "" for the usual by-the-date behaviour. Checked here so a
    // typo is reported rather than silently ignored; the renderer falls back anyway if a theme is ever removed.
    {
      char ft[16] = "";
      bool ht = false;
      if (!getStr(o, "force_theme", ft, ht, err)) return false;
      if (ht) {
        if (ft[0] && !themes::byId(ft)) { err = "display.force_theme: unknown theme id"; return false; }
        strlcpy(d.force_theme, ft, sizeof(d.force_theme));
        t = true;
      }
    }
    if (!getBool(o, "moon_page", d.moon_page, t, err)) return false;
    JsonObjectConst s = o["schedule"];
    if (!s.isNull()) {
      if (!getBool(s, "enabled", d.schedule.enabled, t, err)) return false;
      if (!getBool(s, "follow_sun", d.schedule.follow_sun, t, err)) return false;
      if (!getNum(s, "sun_offset_min", d.schedule.sun_offset_min, t, err, -180, 180)) return false;
      if (!getHHMM(s, "day_start", d.schedule.day_start, t, err)) return false;
      if (!getNum(s, "day_level", d.schedule.day_level, t, err, 1, 255)) return false;
      if (!getHHMM(s, "night_start", d.schedule.night_start, t, err)) return false;
      if (!getNum(s, "night_level", d.schedule.night_level, t, err, 1, 255)) return false;
    }
    JsonObjectConst nm = o["night_mode"];
    if (!nm.isNull()) {
      if (!getBool(nm, "enabled", d.night.enabled, t, err)) return false;
      if (!getHHMM(nm, "start", d.night.start, t, err)) return false;
      if (!getHHMM(nm, "end", d.night.end, t, err)) return false;
      if (!getNum(nm, "level", d.night.level, t, err, 1, 255)) return false;
      if (!getBool(nm, "hide_bottom", d.night.hide_bottom, t, err)) return false;
    }
    JsonObjectConst col = o["colors"];
    if (!col.isNull()) {
      if (!getColor(col, "time", d.colors.time, t, err)) return false;
      if (!getColor(col, "date", d.colors.date, t, err)) return false;
      if (!getColor(col, "temp", d.colors.temp, t, err)) return false;
      if (!getColor(col, "text", d.colors.text, t, err)) return false;
      if (!getColor(col, "hi", d.colors.hi, t, err)) return false;
      if (!getColor(col, "lo", d.colors.lo, t, err)) return false;
    }
    if (!getNum(o, "sleep_fade_sec", d.sleep_fade_sec, t, err, 0, 120)) return false;
    JsonVariantConst hol = o["holidays"];
    if (!hol.isNull()) {
      if (!hol.is<JsonArrayConst>()) { err = "display.holidays: expected an array"; return false; }
      uint8_t n = 0;
      for (JsonObjectConst h : hol.as<JsonArrayConst>()) {
        if (n >= MAX_CUSTOM_HOLIDAYS) break;
        HolidayConfig& x = d.holidays[n];
        x = HolidayConfig();
        bool tt = false;
        if (!getNum(h, "month", x.month, tt, err, 1, 12)) return false;
        if (!getNum(h, "day", x.day, tt, err, 1, 31)) return false;
        if (!getStr(h, "label", x.label, tt, err)) return false;
        if (!getColor(h, "color", x.color, tt, err)) return false;
        if (x.month && x.day) n++;
      }
      d.nholidays = n;
      t = true;
    }
    if (t) changed |= CHG_DISPLAY;
  }

  o = src["panel"];
  if (!o.isNull()) {
    t = false;
    PanelConfig& p = c.panel;
    if (!getNum(o, "width", p.width, t, err, 32, 128)) return false;
    if (!getNum(o, "height", p.height, t, err, 16, 64)) return false;
    if (!getNum(o, "chain", p.chain, t, err, 1, 4)) return false;
    JsonVariantConst dr = o["driver"];
    if (!dr.isNull()) {
      if (!panel_driver_parse(dr.as<const char*>(), p.driver)) { err = "driver: unknown driver name"; return false; }
      t = true;
    }
    if (!getBool(o, "clkphase", p.clkphase, t, err)) return false;
    if (!getNum(o, "latch_blanking", p.latch_blanking, t, err, 1, 4)) return false;
    if (!getNum(o, "i2s_speed_hz", p.i2s_speed_hz, t, err, 1000000, 20000000)) return false;
    if (!getNum(o, "min_refresh_hz", p.min_refresh_hz, t, err, 30, 250)) return false;
    if (!getNum(o, "max_brightness", p.max_brightness, t, err, 1, 255)) return false;
    if (!getNum(o, "color_depth_bits", p.color_depth_bits, t, err, 4, 8)) return false;
    if (!getBool(o, "double_buffer", p.double_buffer, t, err)) return false;
    if (!getBool(o, "swap_rb", p.swap_rb, t, err)) return false;
    if (t) changed |= CHG_PANEL;
  }

  o = src["audio"];
  if (!o.isNull()) {
    t = false;
    AudioConfig& a = c.audio;
    if (!getBool(o, "enabled", a.enabled, t, err)) return false;
    if (!getNum(o, "volume", a.volume, t, err, 0, 100)) return false;
    JsonVariantConst ch = o["chime"];
    if (!ch.isNull()) {
      if (!chime_parse(ch.as<const char*>(), a.chime)) { err = "chime: unknown style"; return false; }
      t = true;
    }
    JsonVariantConst chx = o["chime_extreme"];
    if (!chx.isNull()) {
      if (!chime_parse(chx.as<const char*>(), a.chime_extreme)) { err = "chime_extreme: unknown style"; return false; }
      t = true;
    }
    if (!getNum(o, "repeat_min", a.repeat_min, t, err, 0, 120)) return false;
    if (!getBool(o, "hourly_chime", a.hourly_chime, t, err)) return false;
    if (!getBool(o, "hourly_strike", a.hourly_strike, t, err)) return false;
    if (!getBool(o, "hourly_half", a.hourly_half, t, err)) return false;
    if (!getBool(o, "hourly_speak", a.hourly_speak, t, err)) return false;
    JsonVariantConst chh = o["hourly_style"];
    if (!chh.isNull()) {
      if (!chime_parse(chh.as<const char*>(), a.hourly_style)) { err = "hourly_style: unknown style"; return false; }
      t = true;
    }
    JsonObjectConst q = o["quiet"];
    if (!q.isNull()) {
      if (!getBool(q, "enabled", a.quiet.enabled, t, err)) return false;
      if (!getHHMM(q, "start", a.quiet.start, t, err)) return false;
      if (!getHHMM(q, "end", a.quiet.end, t, err)) return false;
    }
    JsonObjectConst sp = o["speech"];
    if (!sp.isNull()) {
      if (!getBool(sp, "enabled", a.speech.enabled, t, err)) return false;
      if (!getBool(sp, "alerts", a.speech.alerts, t, err)) return false;
      if (!getBool(sp, "lightning", a.speech.lightning, t, err)) return false;
      if (!getBool(sp, "alarms", a.speech.alarms, t, err)) return false;
      if (!getBool(sp, "demo", a.speech.demo, t, err)) return false;
      if (!getBool(sp, "indoor", a.speech.indoor, t, err)) return false;
      if (!getBool(sp, "say_time", a.speech.say_time, t, err)) return false;
      if (!getNum(sp, "repeat", a.speech.repeat, t, err, 1, 3)) return false;
    }
    if (t) changed |= CHG_AUDIO;
  }

  o = src["lightning"];
  if (!o.isNull()) {
    t = false;
    LightningConfig& l = c.lightning;
    if (!getBool(o, "enabled", l.enabled, t, err)) return false;
    if (!getStr(o, "server", l.server, t, err, false)) return false;
    if (!getNum(o, "port", l.port, t, err, 1, 65535)) return false;
    if (!getNum(o, "radius_km", l.radius_km, t, err, 5, 300)) return false;
    if (!getNum(o, "window_min", l.window_min, t, err, 1, 120)) return false;
    if (!getBool(o, "chime", l.chime, t, err)) return false;
    if (!getBool(o, "show_bolt", l.show_bolt, t, err)) return false;
    if (t) changed |= CHG_LIGHTNING;
  }

  o = src["pushbullet"];
  if (!o.isNull()) {
    t = false;
    PushbulletConfig& pb = c.pushbullet;
    JsonVariantConst tk = o["token"];
    if (tk.is<const char*>() && strcmp(tk.as<const char*>(), "***") != 0) {
      if (!getStr(o, "token", pb.token, t, err)) return false;
      pb.device_iden[0] = '\0';   // new account: register again
    }
    if (!getStr(o, "device_iden", pb.device_iden, t, err)) return false;
    if (!getBool(o, "notify_alerts", pb.notify_alerts, t, err)) return false;
    if (!getSeverity(o, "notify_min_severity", pb.notify_min_severity, t, err)) return false;
    if (!getBool(o, "notify_lightning", pb.notify_lightning, t, err)) return false;
    if (!getBool(o, "notify_alarms", pb.notify_alarms, t, err)) return false;
    if (!getBool(o, "notify_air", pb.notify_air, t, err)) return false;
    if (!getBool(o, "show_pushes", pb.show_pushes, t, err)) return false;
    if (!getNum(o, "poll_sec", pb.poll_sec, t, err, 15, 3600)) return false;
    if (!getNum(o, "show_sec", pb.show_sec, t, err, 0, 3600)) return false;
    if (!getBool(o, "chime", pb.chime, t, err)) return false;
    if (t) changed |= CHG_PUSHBULLET;
  }

  o = src["update"];
  if (!o.isNull()) {
    t = false;
    if (!getBool(o, "check", c.update.check, t, err)) return false;
    if (!getBool(o, "auto_install", c.update.auto_install, t, err)) return false;
    if (!getStr(o, "url", c.update.url, t, err, false)) return false;
    if (strncmp(c.update.url, "https://", 8) != 0 && strncmp(c.update.url, "http://", 7) != 0) { err = "update.url: must start with http:// or https://"; return false; }
    if (!strcmp(c.update.url, "https://icbizlabs.github.io/MatrixClock/manifest.json")) {   // repository renamed in 0.9.2; the old Pages address is gone
      strlcpy(c.update.url, "https://icbizlabs.github.io/MatrixWeatherClock/manifest.json", sizeof(c.update.url));
      t = true;
    }
    if (!getNum(o, "check_hours", c.update.check_hours, t, err, 1, 168)) return false;
    if (t) changed |= CHG_UPDATE;
  }

  o = src["indoor"];
  if (!o.isNull()) {
    t = false;
    IndoorConfig& in = c.indoor;
    if (!getBool(o, "enabled", in.enabled, t, err)) return false;
    if (!getBool(o, "auto_page", in.auto_page, t, err)) return false;
    if (!getNum(o, "sample_sec", in.sample_sec, t, err, 2, 600)) return false;
    if (!getNum(o, "temp_offset", in.temp_offset, t, err, -30, 30)) return false;
    if (!getNum(o, "humidity_offset", in.humidity_offset, t, err, -50, 50)) return false;
    if (!getNum(o, "altitude_m", in.altitude_m, t, err, -1, 9000)) return false;
    if (!getBool(o, "sea_level", in.sea_level, t, err)) return false;
    JsonVariantConst pu = o["pressure_unit"];
    if (!pu.isNull()) {
      const char* u = pu | "auto";
      if (!strcmp(u, "auto")) in.pressure_unit = 0; else if (!strcmp(u, "hpa")) in.pressure_unit = 1; else if (!strcmp(u, "inhg")) in.pressure_unit = 2;
      else { err = "indoor.pressure_unit: auto, hpa or inhg"; return false; }
      t = true;
    }
    if (!getNum(o, "trend_min", in.trend_min, t, err, 10, 1440)) return false;
    if (!getNum(o, "pressure_trend_min", in.pressure_trend_min, t, err, 30, 1440)) return false;
    if (!getBool(o, "gas", in.gas, t, err)) return false;
    if (!getNum(o, "air_fair_below", in.air_fair_below, t, err, 1, 100)) return false;
    if (!getNum(o, "air_poor_below", in.air_poor_below, t, err, 1, 100)) return false;
    if (in.air_poor_below >= in.air_fair_below) { err = "indoor.air_poor_below must be below air_fair_below"; return false; }
    if (!getBool(o, "air_alert", in.air_alert, t, err)) return false;
    if (!getNum(o, "air_alert_min", in.air_alert_min, t, err, 5, 1440)) return false;
    if (t) changed |= CHG_INDOOR;
  }
  c.indoor.temp_offset_c = c.weather.imperial ? c.indoor.temp_offset * 5.0f / 9.0f : c.indoor.temp_offset;

  o = src["tide"];
  if (!o.isNull()) {
    t = false;
    TideConfig& td = c.tide;
    if (!getBool(o, "enabled", td.enabled, t, err)) return false;
    if (!getBool(o, "auto_page", td.auto_page, t, err)) return false;
    JsonVariantConst stn = o["station"];
    if (!stn.isNull()) {
      const char* v = stn | "";
      for (const char* q = v; *q; q++) if (!isdigit((unsigned char)*q)) { err = "tide.station: NOAA station id (digits)"; return false; }
      strlcpy(td.station, v, sizeof(td.station));
      t = true;
    }
    JsonVariantConst sn = o["station_name"];
    if (!sn.isNull()) { strlcpy(td.station_name, sn | "", sizeof(td.station_name)); t = true; }
    JsonVariantConst un = o["unit"];
    if (!un.isNull()) {
      const char* v = un | "auto";
      if (!strcmp(v, "auto")) td.unit = 0; else if (!strcmp(v, "ft")) td.unit = 1; else if (!strcmp(v, "m")) td.unit = 2;
      else { err = "tide.unit: auto, ft or m"; return false; }
      t = true;
    }
    if (!getNum(o, "refresh_hours", td.refresh_hours, t, err, 1, 24)) return false;
    if (!getBool(o, "water_temp", td.water_temp, t, err)) return false;
    if (t) changed |= CHG_TIDE;
  }

  o = src["remote"];
  if (!o.isNull()) {
    t = false;
    RemoteConfig& rm = c.remote;
    if (!getBool(o, "enabled", rm.enabled, t, err)) return false;
    if (!getNum(o, "pin", rm.pin, t, err, 0, 48)) return false;
    JsonVariantConst bt = o["buttons"];
    if (!bt.isNull()) {
      if (!bt.is<JsonArrayConst>()) { err = "remote.buttons: expected an array"; return false; }
      uint8_t n = 0;
      for (JsonObjectConst b : bt.as<JsonArrayConst>()) {
        if (n >= MAX_REMOTE_BUTTONS) break;
        uint32_t code = 0;
        JsonVariantConst cv = b["code"];
        if (cv.is<const char*>()) code = (uint32_t)strtoul(cv.as<const char*>(), nullptr, 0);
        else code = cv | 0UL;
        actions::Id id = actions::Id::None;
        const char* an = b["action"] | "none";
        if (!actions::parse(an, id)) { err = String("remote.buttons: unknown action ") + an; return false; }
        if (!code || id == actions::Id::None) continue;
        rm.buttons[n].code = code;
        rm.buttons[n].action = (uint8_t)id;
        n++;
      }
      rm.nbuttons = n;
      t = true;
    }
    if (t) changed |= CHG_REMOTE;
  }

  o = src["radar"];
  if (!o.isNull()) {
    t = false;
    RadarConfig& r = c.radar;
    if (!getBool(o, "enabled", r.enabled, t, err)) return false;
    if (!getNum(o, "radius_km", r.radius_km, t, err, 25, 400)) return false;
    if (!getNum(o, "every_n_cycles", r.every_n_cycles, t, err, 1, 20)) return false;
    if (!getBool(o, "show_when_precip", r.show_when_precip, t, err)) return false;
    if (!getNum(o, "precip_every_n_cycles", r.precip_every_n_cycles, t, err, 1, 20)) return false;
    if (!getNum(o, "frame_ms", r.frame_ms, t, err, 100, 2000)) return false;
    if (!getNum(o, "hold_ms", r.hold_ms, t, err, 0, 5000)) return false;
    if (!getNum(o, "show_sec", r.show_sec, t, err, 4, 60)) return false;
    if (!getNum(o, "refresh_min", r.refresh_min, t, err, 2, 30)) return false;
    JsonVariantConst rsrc = o["source"];
    if (!rsrc.isNull()) {
      const char* v = rsrc | "nowcoast";
      if (!strcmp(v, "nowcoast")) r.source = 0; else if (!strcmp(v, "mesonet")) r.source = 1;
      else { err = "radar.source: nowcoast or mesonet"; return false; }
      t = true;
    }
    JsonVariantConst bm = o["base_map"];
    if (!bm.isNull()) {
      const char* m = bm | "both";
      if (!strcmp(m, "none")) r.base_map = 0; else if (!strcmp(m, "coast")) r.base_map = 1; else if (!strcmp(m, "landwater")) r.base_map = 2; else if (!strcmp(m, "both")) r.base_map = 3;
      else { err = "radar.base_map: none, coast, landwater or both"; return false; }
      t = true;
    }
    if (t) changed |= CHG_RADAR;
  }

  o = src["webhook"];
  if (!o.isNull()) {
    t = false;
    WebhookConfig& w = c.webhook;
    if (!getBool(o, "enabled", w.enabled, t, err)) return false;
    if (!getStr(o, "url", w.url, t, err)) return false;
    if (w.url[0] && strncmp(w.url, "http://", 7) && strncmp(w.url, "https://", 8)) { err = "webhook.url: must start with http:// or https://"; return false; }
    if (w.enabled && !w.url[0]) { err = "webhook.url: required when the webhook is enabled"; return false; }
    if (!getStr(o, "title_key", w.title_key, t, err, false)) return false;
    if (!getStr(o, "body_key", w.body_key, t, err, false)) return false;
    if (!getStr(o, "header_name", w.header_name, t, err)) return false;
    JsonVariantConst hv = o["header_value"];
    if (hv.is<const char*>() && strcmp(hv.as<const char*>(), "***") != 0) {
      if (!getStr(o, "header_value", w.header_value, t, err)) return false;
    }
    JsonVariantConst ms = o["min_severity"];
    if (!ms.isNull()) {
      if (!severity_parse(ms.as<const char*>(), w.min_severity)) { err = "webhook.min_severity: unknown severity"; return false; }
      t = true;
    }
    if (!getBool(o, "on_alerts", w.on_alerts, t, err)) return false;
    if (!getBool(o, "on_lightning", w.on_lightning, t, err)) return false;
    if (!getBool(o, "on_alarms", w.on_alarms, t, err)) return false;
    if (!getBool(o, "on_air", w.on_air, t, err)) return false;
    if (!getBool(o, "on_boot", w.on_boot, t, err)) return false;
    if (t) changed |= CHG_WEBHOOK;
  }

  JsonVariantConst ev = src["events"];
  if (!ev.isNull()) {
    if (!ev.is<JsonArrayConst>()) { err = "events: expected an array"; return false; }
    uint8_t n = 0;
    for (JsonObjectConst e : ev.as<JsonArrayConst>()) {
      if (n >= MAX_EVENTS) break;
      EventConfig& x = c.events.items[n];
      x = EventConfig();
      bool tt = false;
      if (!getBool(e, "enabled", x.enabled, tt, err)) return false;
      if (!getStr(e, "label", x.label, tt, err)) return false;
      if (!getBool(e, "yearly", x.yearly, tt, err)) return false;
      if (!getNum(e, "year", x.year, tt, err, 0, 2200)) return false;
      if (!getNum(e, "month", x.month, tt, err, 0, 12)) return false;
      if (!getNum(e, "day", x.day, tt, err, 0, 31)) return false;
      if (!x.yearly && x.enabled && x.year < 2024) { err = "events.year: a one-off event needs a full year"; return false; }
      n++;
    }
    for (uint8_t i = n; i < MAX_EVENTS; i++) c.events.items[i] = EventConfig();
    changed |= CHG_EVENT;
  }

  JsonVariantConst wo = src["world"];
  if (!wo.isNull()) {
    t = false;
    JsonObjectConst w = wo.as<JsonObjectConst>();
    if (w.isNull()) { err = "world: expected an object"; return false; }
    if (!getBool(w, "enabled", c.world.enabled, t, err)) return false;
    if (!getNum(w, "every_n_cycles", c.world.every_n_cycles, t, err, 1, 60)) return false;
    if (!getNum(w, "show_sec", c.world.show_sec, t, err, 3, 60)) return false;
    JsonVariantConst zs = w["zones"];
    if (!zs.isNull()) {
      if (!zs.is<JsonArrayConst>()) { err = "world.zones: expected an array"; return false; }
      uint8_t n = 0;
      for (JsonObjectConst e : zs.as<JsonArrayConst>()) {
        if (n >= MAX_WORLD_ZONES) break;
        WorldZone& x = c.world.zones[n];
        x = WorldZone();
        bool tt = false;
        if (!getStr(e, "id", x.id, tt, err)) return false;
        if (!getStr(e, "posix", x.posix, tt, err)) return false;
        if (!getStr(e, "label", x.label, tt, err)) return false;
        n++;
      }
      for (uint8_t i = n; i < MAX_WORLD_ZONES; i++) c.world.zones[i] = WorldZone();
      t = true;
    }
    if (t) changed |= CHG_WORLD;
  }

  JsonVariantConst al = src["alarms"];
  if (!al.isNull()) {
    if (!al.is<JsonArrayConst>()) { err = "alarms: expected an array"; return false; }
    uint8_t i = 0;
    for (JsonObjectConst a : al.as<JsonArrayConst>()) {
      if (i >= MAX_ALARMS) break;
      AlarmConfig& x = c.alarms.items[i];
      bool tt = false;
      if (!getBool(a, "enabled", x.enabled, tt, err)) return false;
      if (!getHHMM(a, "time", x.minute, tt, err)) return false;
      JsonVariantConst dv = a["days"];
      if (dv.is<const char*>()) {
        const char* ds = dv.as<const char*>();
        if (strlen(ds) != 7) { err = "alarms.days: expected 7 characters (Mon..Sun, 0/1)"; return false; }
        uint8_t m = 0;
        for (int k = 0; k < 7; k++) if (ds[k] == '1') m |= (1 << k);
        x.days = m;
      }
      JsonVariantConst ch = a["chime"];
      if (!ch.isNull() && !chime_parse(ch.as<const char*>(), x.chime)) { err = "alarms.chime: unknown style"; return false; }
      if (!getStr(a, "label", x.label, tt, err)) return false;
      if (!getBool(a, "once", x.once, tt, err)) return false;
      if (!getNum(a, "snooze_min", x.snooze_min, tt, err, 1, 60)) return false;
      i++;
    }
    changed |= CHG_ALARMS;
  }

  JsonVariantConst fb = src["first_boot"];
  if (fb.is<bool>()) c.first_boot = fb.as<bool>();
  return true;
}

void config_to_json(const AppConfig& c, JsonObject dst, bool mask_secrets) {
  dst["version"] = 1;
  JsonObject o = dst["wifi"].to<JsonObject>();
  o["ssid"] = c.wifi.ssid;
  o["pass"] = mask_secrets ? (c.wifi.pass[0] ? "***" : "") : c.wifi.pass;
  o["hostname"] = c.wifi.hostname;
  o["ap_pass"] = mask_secrets ? (c.wifi.ap_pass[0] ? "***" : "") : c.wifi.ap_pass;
  o["tx_power"] = c.wifi.tx_power;
  o["ssid2"] = c.wifi.ssid2;
  o["pass2"] = mask_secrets ? (c.wifi.pass2[0] ? "***" : "") : c.wifi.pass2;
  o["dhcp"] = c.wifi.dhcp;
  o["ip"] = c.wifi.ip;
  o["gateway"] = c.wifi.gateway;
  o["netmask"] = c.wifi.netmask;
  o["dns"] = c.wifi.dns;

  o = dst["location"].to<JsonObject>();
  o["lat"] = c.location.lat;
  o["lon"] = c.location.lon;
  o["name"] = c.location.name;

  o = dst["time"].to<JsonObject>();
  o["tz_id"] = c.time.tz_id;
  o["tz_posix"] = c.time.tz_posix;
  o["ntp1"] = c.time.ntp1;
  o["ntp2"] = c.time.ntp2;
  o["use_24h"] = c.time.use_24h;
  o["show_seconds"] = c.time.show_seconds;
  o["date_order"] = c.time.date_order == 1 ? "dmy" : c.time.date_order == 2 ? "iso" : "mdy";

  o = dst["weather"].to<JsonObject>();
  o["enabled"] = c.weather.enabled;
  o["units"] = c.weather.imperial ? "imperial" : "metric";
  o["refresh_min"] = c.weather.refresh_min;
  o["forecast_days"] = c.weather.forecast_days;

  o = dst["alerts"].to<JsonObject>();
  o["enabled"] = c.alerts.enabled;
  o["poll_sec"] = c.alerts.poll_sec;
  o["user_agent_contact"] = c.alerts.user_agent_contact;
  o["min_severity"] = severity_name(c.alerts.min_severity);
  JsonArray ig = o["ignored_events"].to<JsonArray>();
  {
    char buf[sizeof(c.alerts.ignored_events)];
    strlcpy(buf, c.alerts.ignored_events, sizeof(buf));
    char* save = nullptr;
    for (char* tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(nullptr, ",", &save)) {
      while (*tok == ' ') tok++;
      if (*tok) ig.add(String(tok));
    }
  }
  o["chime_min_severity"] = severity_name(c.alerts.chime_min_severity);
  o["flash_frame_sec"] = c.alerts.flash_frame_sec;
  o["banner_px_per_s"] = c.alerts.banner_px_per_s;
  o["tls_verify"] = c.alerts.tls_verify;

  o = dst["display"].to<JsonObject>();
  const DisplayConfig& d = c.display;
  o["brightness"] = d.brightness;
  o["gamma"] = d.gamma;
  o["page_sec"] = d.page_sec;
  JsonArray pg = o["pages"].to<JsonArray>();
  for (uint8_t i = 0; i < d.npages; i++) pg.add(page_name(d.pages[i]));
  o["forecast_page"] = d.forecast_page;
  o["forecast_every_n_cycles"] = d.forecast_every_n_cycles;
  o["colon_blink"] = d.colon_blink;
  o["ip_on_connect_sec"] = d.ip_on_connect_sec;
  o["hourly_page"] = d.hourly_page;
  o["transitions"] = d.transitions;
  o["precip_fx"] = d.precip_fx;
  o["holiday_themes"] = d.holiday_themes;
  o["holidays_enabled"] = d.holidays_enabled;
  o["force_theme"] = d.force_theme;
  o["moon_page"] = d.moon_page;
  o["sleep_fade_sec"] = d.sleep_fade_sec;
  {
    JsonArray hl = o["holidays"].to<JsonArray>();
    for (uint8_t i = 0; i < d.nholidays; i++) {
      JsonObject h = hl.add<JsonObject>();
      h["month"] = d.holidays[i].month;
      h["day"] = d.holidays[i].day;
      h["label"] = d.holidays[i].label;
      char col[8];
      snprintf(col, sizeof(col), "#%06lX", (unsigned long)(d.holidays[i].color & 0xFFFFFF));
      h["color"] = col;
    }
  }
  JsonObject s = o["schedule"].to<JsonObject>();
  s["enabled"] = d.schedule.enabled;
  s["follow_sun"] = d.schedule.follow_sun;
  s["sun_offset_min"] = d.schedule.sun_offset_min;
  putHHMM(s, "day_start", d.schedule.day_start);
  s["day_level"] = d.schedule.day_level;
  putHHMM(s, "night_start", d.schedule.night_start);
  s["night_level"] = d.schedule.night_level;
  JsonObject nm = o["night_mode"].to<JsonObject>();
  nm["enabled"] = d.night.enabled;
  putHHMM(nm, "start", d.night.start);
  putHHMM(nm, "end", d.night.end);
  nm["level"] = d.night.level;
  nm["hide_bottom"] = d.night.hide_bottom;
  JsonObject col = o["colors"].to<JsonObject>();
  putColor(col, "time", d.colors.time);
  putColor(col, "date", d.colors.date);
  putColor(col, "temp", d.colors.temp);
  putColor(col, "text", d.colors.text);
  putColor(col, "hi", d.colors.hi);
  putColor(col, "lo", d.colors.lo);

  o = dst["panel"].to<JsonObject>();
  const PanelConfig& p = c.panel;
  o["width"] = p.width;
  o["height"] = p.height;
  o["chain"] = p.chain;
  o["driver"] = panel_driver_name(p.driver);
  o["clkphase"] = p.clkphase;
  o["latch_blanking"] = p.latch_blanking;
  o["i2s_speed_hz"] = p.i2s_speed_hz;
  o["min_refresh_hz"] = p.min_refresh_hz;
  o["max_brightness"] = p.max_brightness;
  o["color_depth_bits"] = p.color_depth_bits;
  o["double_buffer"] = p.double_buffer;
  o["swap_rb"] = p.swap_rb;

  o = dst["audio"].to<JsonObject>();
  const AudioConfig& a = c.audio;
  o["enabled"] = a.enabled;
  o["volume"] = a.volume;
  o["chime"] = chime_name(a.chime);
  o["chime_extreme"] = chime_name(a.chime_extreme);
  o["repeat_min"] = a.repeat_min;
  JsonObject q = o["quiet"].to<JsonObject>();
  q["enabled"] = a.quiet.enabled;
  putHHMM(q, "start", a.quiet.start);
  putHHMM(q, "end", a.quiet.end);
  JsonObject sp = o["speech"].to<JsonObject>();
  sp["enabled"] = a.speech.enabled;
  sp["alerts"] = a.speech.alerts;
  sp["lightning"] = a.speech.lightning;
  sp["alarms"] = a.speech.alarms;
  sp["demo"] = a.speech.demo;
  sp["indoor"] = a.speech.indoor;
  sp["say_time"] = a.speech.say_time;
  sp["repeat"] = a.speech.repeat;

  o = dst["lightning"].to<JsonObject>();
  o["enabled"] = c.lightning.enabled;
  o["server"] = c.lightning.server;
  o["port"] = c.lightning.port;
  o["radius_km"] = c.lightning.radius_km;
  o["window_min"] = c.lightning.window_min;
  o["chime"] = c.lightning.chime;
  o["show_bolt"] = c.lightning.show_bolt;

  o = dst["pushbullet"].to<JsonObject>();
  o["token"] = mask_secrets ? (c.pushbullet.token[0] ? "***" : "") : c.pushbullet.token;
  o["device_iden"] = c.pushbullet.device_iden;
  o["notify_alerts"] = c.pushbullet.notify_alerts;
  o["notify_min_severity"] = severity_name(c.pushbullet.notify_min_severity);
  o["notify_lightning"] = c.pushbullet.notify_lightning;
  o["notify_alarms"] = c.pushbullet.notify_alarms;
  o["notify_air"] = c.pushbullet.notify_air;
  o["show_pushes"] = c.pushbullet.show_pushes;
  o["poll_sec"] = c.pushbullet.poll_sec;
  o["show_sec"] = c.pushbullet.show_sec;
  o["chime"] = c.pushbullet.chime;

  o = dst["update"].to<JsonObject>();
  o["check"] = c.update.check;
  o["auto_install"] = c.update.auto_install;
  o["url"] = c.update.url;
  o["check_hours"] = c.update.check_hours;

  o = dst["indoor"].to<JsonObject>();
  o["enabled"] = c.indoor.enabled;
  o["auto_page"] = c.indoor.auto_page;
  o["sample_sec"] = c.indoor.sample_sec;
  o["temp_offset"] = c.indoor.temp_offset;
  o["humidity_offset"] = c.indoor.humidity_offset;
  o["altitude_m"] = c.indoor.altitude_m;
  o["sea_level"] = c.indoor.sea_level;
  o["pressure_unit"] = c.indoor.pressure_unit == 1 ? "hpa" : c.indoor.pressure_unit == 2 ? "inhg" : "auto";
  o["trend_min"] = c.indoor.trend_min;
  o["pressure_trend_min"] = c.indoor.pressure_trend_min;
  o["gas"] = c.indoor.gas;
  o["air_fair_below"] = c.indoor.air_fair_below;
  o["air_poor_below"] = c.indoor.air_poor_below;
  o["air_alert"] = c.indoor.air_alert;
  o["air_alert_min"] = c.indoor.air_alert_min;

  o = dst["tide"].to<JsonObject>();
  o["enabled"] = c.tide.enabled;
  o["auto_page"] = c.tide.auto_page;
  o["station"] = c.tide.station;
  o["station_name"] = c.tide.station_name;
  o["unit"] = c.tide.unit == 1 ? "ft" : c.tide.unit == 2 ? "m" : "auto";
  o["refresh_hours"] = c.tide.refresh_hours;
  o["water_temp"] = c.tide.water_temp;

  o = dst["remote"].to<JsonObject>();
  o["enabled"] = c.remote.enabled;
  o["pin"] = c.remote.pin;
  {
    JsonArray bt = o["buttons"].to<JsonArray>();
    for (uint8_t i = 0; i < c.remote.nbuttons; i++) {
      JsonObject b = bt.add<JsonObject>();
      char hex[12];
      snprintf(hex, sizeof(hex), "0x%08lX", (unsigned long)c.remote.buttons[i].code);
      b["code"] = hex;
      b["action"] = actions::name((actions::Id)c.remote.buttons[i].action);
    }
  }

  o = dst["radar"].to<JsonObject>();
  o["enabled"] = c.radar.enabled;
  o["radius_km"] = c.radar.radius_km;
  o["every_n_cycles"] = c.radar.every_n_cycles;
  o["show_when_precip"] = c.radar.show_when_precip;
  o["precip_every_n_cycles"] = c.radar.precip_every_n_cycles;
  o["frame_ms"] = c.radar.frame_ms;
  o["hold_ms"] = c.radar.hold_ms;
  o["show_sec"] = c.radar.show_sec;
  o["refresh_min"] = c.radar.refresh_min;
  o["base_map"] = c.radar.base_map == 0 ? "none" : c.radar.base_map == 1 ? "coast" : c.radar.base_map == 2 ? "landwater" : "both";
  o["source"] = c.radar.source == 1 ? "mesonet" : "nowcoast";

  JsonArray al = dst["alarms"].to<JsonArray>();
  for (uint8_t i = 0; i < MAX_ALARMS; i++) {
    const AlarmConfig& x = c.alarms.items[i];
    JsonObject a = al.add<JsonObject>();
    a["enabled"] = x.enabled;
    putHHMM(a, "time", x.minute);
    char ds[8];
    for (int k = 0; k < 7; k++) ds[k] = (x.days & (1 << k)) ? '1' : '0';
    ds[7] = '\0';
    a["days"] = ds;
    a["chime"] = chime_name(x.chime);
    a["label"] = x.label;
    a["once"] = x.once;
    a["snooze_min"] = x.snooze_min;
  }

  o = dst["webhook"].to<JsonObject>();
  o["enabled"] = c.webhook.enabled;
  o["url"] = c.webhook.url;
  o["title_key"] = c.webhook.title_key;
  o["body_key"] = c.webhook.body_key;
  o["header_name"] = c.webhook.header_name;
  o["header_value"] = mask_secrets ? (c.webhook.header_value[0] ? "***" : "") : c.webhook.header_value;
  o["on_alerts"] = c.webhook.on_alerts;
  o["min_severity"] = severity_name(c.webhook.min_severity);
  o["on_lightning"] = c.webhook.on_lightning;
  o["on_alarms"] = c.webhook.on_alarms;
  o["on_air"] = c.webhook.on_air;
  o["on_boot"] = c.webhook.on_boot;

  {
    JsonArray evs = dst["events"].to<JsonArray>();
    for (uint8_t i = 0; i < MAX_EVENTS; i++) {
      const EventConfig& x = c.events.items[i];
      JsonObject e = evs.add<JsonObject>();
      e["enabled"] = x.enabled;
      e["label"] = x.label;
      e["yearly"] = x.yearly;
      e["year"] = x.year;
      e["month"] = x.month;
      e["day"] = x.day;
    }
  }

  {
    JsonObject w = dst["world"].to<JsonObject>();
    w["enabled"] = c.world.enabled;
    w["every_n_cycles"] = c.world.every_n_cycles;
    w["show_sec"] = c.world.show_sec;
    JsonArray zs = w["zones"].to<JsonArray>();
    for (uint8_t i = 0; i < MAX_WORLD_ZONES; i++) {
      const WorldZone& x = c.world.zones[i];
      JsonObject e = zs.add<JsonObject>();
      e["id"] = x.id;
      e["posix"] = x.posix;
      e["label"] = x.label;
    }
  }

  dst["first_boot"] = c.first_boot;
}
