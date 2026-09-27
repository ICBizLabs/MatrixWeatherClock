# REST API

All endpoints answer JSON unless noted. The web UI uses nothing else.

| Method | Path | Description |
|---|---|---|
| GET | `/` | Web UI (gzip, ETag) |
| GET | `/setup` | mobile setup wizard (WiFi, location, time zone, alert contact) |
| GET | `/manifest.webmanifest`, `/sw.js`, `/icon-192.png`, `/icon-512.png`, `/apple-touch-icon.png` | web-app manifest, service worker and icons for "Add to Home Screen" |
| GET | `/api/status` | time, weather summary, moon phase, active alerts, WiFi, network fetch status, memory, panel, audio, speech (voice pack state), indoor sensor (values, trends), I2C map with every address found |
| GET | `/api/config` | full configuration (passwords masked as `***`) plus `tz_options` and `pages_available` (every page id with a human label, for the web UI's page picker) |
| POST | `/api/config` | partial configuration merge; body is any subset of the config object. Returns `{ok, applied[], reboot_required[]}` or `{ok:false, error}` with HTTP 400 |
| GET | `/api/weather` | last Open-Meteo result |
| GET | `/api/alerts[?all=1]` | active alerts (filtered by severity/ignore list unless `all=1`) |
| POST | `/api/alerts/ack` | form or query `id=<alert id>` or `id=all` |
| POST | `/api/test/alert` | `{event, severity, headline, minutes}` injects a synthetic alert |
| POST | `/api/test/chime` | `{style?, force}` plays a chime (`force` ignores quiet hours) |
| POST | `/api/test/say` | `{text, force}` speaks a phrase from the voice pack ("Tornado Warning", "Lightning nearby", ...); 404 when the phrase is not in the pack, 409 without a pack or while another sound plays |
| POST | `/api/voice/download` | fetch the manifest and (re)download the voice pack into LittleFS |
| POST | `/api/action` | `name=<action>` (form or query): runs a remote-control action now; `GET /api/actions` lists `{name, label}` pairs (next_page, dismiss, show_radar, show_forecast, show_hourly, ack_alerts, alarm_stop, alarm_snooze, timer_5/10/30, timer_cancel, bright_up, bright_down, night, mute, demo, refresh, show_ip, chime, say_time, stopwatch, stopwatch_reset, sleep_30, sleep_60, sleep_cancel) |
| GET | `/api/remote` | infrared receiver state: `{enabled, pin, received, learning, last_code, last_proto, last_ms, last_age_s}` |
| POST | `/api/remote/learn` | record the next received code without executing it (the web UI's Learn button) |
| GET | `/remote` | the phone remote page |
| POST | `/api/tide/refresh` | fetch the tide predictions now; `/api/status.tide` has `{enabled, valid, station, unit, rising, height, next_high {t, h}, next_low {t, h}, extremes[]}` with epoch times |
| GET | `/api/radar` | radar loop state: `{enabled, frames, error, echo_near, echo_pct, base, last_ok_age_s, age_min[]}` (`base_map`: `none`, `coast`, `landwater`, `both`; `source`: `nowcoast` (MRMS, quality controlled) or `mesonet` (unfiltered NEXRAD composite)) |
| GET | `/api/radar/frame?i=N` | frame N (0 = oldest) as raw RGB565 little-endian 64x32 (headers `X-Frame-Size`, `X-Frame-Age-Min`) |
| GET | `/api/radar/base` | base map mask, one byte per pixel (bit 0 water, bit 1 land, bit 2 coastline), 64x32 |
| POST | `/api/radar/refresh` | fetch the newest composite now |
| POST | `/api/indoor/rescan` | scan the I2C bus and probe for the sensor again (runs on the next main-loop pass; read `/api/status` a second later) |
| GET | `/api/indoor/history[?minutes=180&step=1]` | indoor sensor history, oldest first: `{sensor, has_gas, step_min, age_min[], temp_c[], humidity[], pressure_hpa[], gas_kohm[], air_score[]}` (up to 1440 minutes) |
| GET | `/api/voice/phrases` | `{installed, voice, version, phrases[]}`: every phrase the installed pack contains (packs are 8-bit µ-law at 22050 Hz; format 1 packs, 4-bit ADPCM, still play) |
| POST | `/api/test/panel` | shows the test pattern; optional `sec=3..300` (default 10) |
| POST | `/api/message` | `{text, seconds (0 = until cleared), color "#RRGGBB", chime, force}` scrolls a message |
| POST | `/api/message/clear` | remove the message |
| GET | `/api/frame` | current panel content as raw RGB565 little-endian (header `X-Frame-Size: 64x32`) |
| POST | `/api/timer` | `{minutes}` or `{seconds}` starts the countdown timer |
| POST | `/api/timer/cancel` | cancel the timer (also silences a finished one) |
| POST | `/api/alarm/stop`, `/api/alarm/snooze` | control a ringing alarm |
| POST | `/api/sleep` | `{minutes}` arms the sleep timer (0 cancels); after that many minutes the panel fades out over `display.sleep_fade_sec` and stays dark. `/api/status.sleep` is `{pending, remaining_s, faded}` |
| POST | `/api/stopwatch` | `{action}` one of `start`, `stop`, `reset`, `toggle`; returns `{ok, running, ms}`. `/api/status.stopwatch` is `{running, active, ms}` |
| POST | `/api/test/say-time` | speaks the current time from the pack's number clips (`?force=1` ignores quiet hours); 409 when the clock is not set, the feature is off, or the pack has no number clips |
| POST | `/api/test/webhook` | post a test notification to the configured webhook URL; 409 when no webhook is configured |
| GET | `/api/config?download=1` | full configuration with passwords, as a downloadable backup |
| POST | `/api/test/push` | send a test Pushbullet notification |
| POST | `/api/demo` | `on=1|0`, `minutes=N`, `sound=1|0`, `start=N` (scenario index to begin with): cycle demo screens with sample data, auto-off after N minutes; with sound the alert, lightning, alarm, timer and message scenarios chime and every scenario says its name |
| POST | `/api/update/check` | check the manifest for a newer version now |
| POST | `/api/update/install` | download, verify and install the available version, then reboot |
| POST | `/api/show` | `screen=forecast`, `screen=hourly` or `screen=radar` shows that full screen now |
| POST | `/api/refresh` | fetch weather and alerts now |
| GET | `/api/wifi/scan[?start=1|?poll=1]` | start / poll an async network scan |
| GET | `/api/log` | text log ring buffer |
| POST | `/api/reboot` | reboot |
| POST | `/api/factory-reset` | form or query `confirm=yes`; erases config and reboots |
| POST | `/update` | multipart firmware upload (`firmware` field); reboots on success |

Configuration keys and defaults:

```json
{ "wifi":     { "ssid": "", "pass": "", "hostname": "matrixweatherclock", "ap_pass": "", "tx_power": 34,
                "ssid2": "", "pass2": "", "dhcp": true, "ip": "", "gateway": "", "netmask": "255.255.255.0", "dns": "" },
  "location": { "lat": 39.7392, "lon": -104.9903, "name": "" },
  "time":     { "tz_id": "America/Denver", "tz_posix": "MST7MDT,M3.2.0,M11.1.0", "ntp1": "pool.ntp.org", "ntp2": "time.nist.gov", "use_24h": false, "show_seconds": false,
                "date_order": "mdy", "tz2_id": "", "tz2_posix": "", "tz2_label": "" },
  "weather":  { "enabled": true, "units": "imperial", "refresh_min": 15, "forecast_days": 3 },
  "alerts":   { "enabled": true, "poll_sec": 120, "user_agent_contact": "", "min_severity": "Moderate", "ignored_events": [], "chime_min_severity": "Severe", "flash_frame_sec": 60, "banner_px_per_s": 20, "tls_verify": false },
  "display":  { "brightness": 128, "gamma": 2.2, "page_sec": 6, "pages": ["date","temp","cond","wind"], "forecast_page": true, "forecast_every_n_cycles": 3, "colon_blink": true, "ip_on_connect_sec": 20,
                "hourly_page": true, "transitions": true, "precip_fx": true, "holiday_themes": true, "moon_page": true, "sleep_fade_sec": 30,
                "holidays": [ { "month": 0, "day": 0, "label": "", "color": "#FFFFFF" }, "... up to 4" ],
                "schedule": { "enabled": true, "follow_sun": false, "sun_offset_min": 30, "day_start": "07:00", "day_level": 160, "night_start": "21:00", "night_level": 40 },
                "night_mode": { "enabled": true, "start": "23:00", "end": "06:00", "level": 8, "hide_bottom": true },
                "colors": { "time": "#FFFFFF", "date": "#80C0FF", "temp": "#FFD060", "text": "#C0C0C0", "hi": "#FF8060", "lo": "#60A0FF" } },
  "panel":    { "width": 64, "height": 32, "chain": 1, "driver": "SHIFTREG", "clkphase": false, "latch_blanking": 2, "i2s_speed_hz": 8000000, "min_refresh_hz": 120, "max_brightness": 255, "color_depth_bits": 8, "double_buffer": false, "swap_rb": false },
  "audio":    { "enabled": true, "volume": 60, "chime": "two_tone", "chime_extreme": "eas_attention", "repeat_min": 0, "quiet": { "enabled": true, "start": "22:00", "end": "07:00" },
                "hourly_chime": false, "hourly_strike": false, "hourly_half": false, "hourly_speak": false, "hourly_style": "doorbell",
                "speech": { "enabled": true, "alerts": true, "lightning": true, "alarms": true, "demo": true, "indoor": true, "say_time": true, "repeat": 1 } },
  "pushbullet": { "token": "", "device_iden": "", "notify_alerts": true, "notify_min_severity": "Severe", "notify_lightning": true,
                  "notify_alarms": false, "notify_air": true, "show_pushes": true, "poll_sec": 60, "show_sec": 60, "chime": true },
  "update":   { "check": true, "auto_install": true, "url": "https://icbizlabs.github.io/MatrixWeatherClock/manifest.json", "check_hours": 6 },
  "radar":    { "enabled": true, "radius_km": 100, "every_n_cycles": 4, "show_when_precip": true, "precip_every_n_cycles": 2, "frame_ms": 350, "hold_ms": 1500, "show_sec": 12, "refresh_min": 5, "base_map": "both", "source": "nowcoast" },
  "tide":     { "enabled": true, "auto_page": true, "station": "8727012", "station_name": "Pithlachascotee River, FL", "unit": "auto", "refresh_hours": 6, "water_temp": true },
  "webhook":  { "enabled": false, "url": "", "title_key": "title", "body_key": "message", "header_name": "", "header_value": "",
                "on_alerts": true, "min_severity": "Severe", "on_lightning": false, "on_alarms": false, "on_air": false, "on_boot": false },
  "events":   [ { "enabled": false, "label": "", "yearly": false, "year": 0, "month": 0, "day": 0 }, "... up to 3" ],
  "remote":   { "enabled": true, "pin": 44, "buttons": [ { "code": "0x00FF629D", "action": "next_page" }, "... up to 24" ] },
  "indoor":   { "enabled": true, "auto_page": true, "sample_sec": 10, "temp_offset": 0, "humidity_offset": 0, "altitude_m": -1, "sea_level": true, "pressure_unit": "auto", "trend_min": 60, "pressure_trend_min": 180,
                "gas": true, "air_fair_below": 80, "air_poor_below": 60, "air_alert": true, "air_alert_min": 60 },
  "lightning": { "enabled": false, "server": "blitzortung.ha.sed.pl", "port": 1883, "radius_km": 40, "window_min": 15, "chime": true, "show_bolt": true },
  "alarms":   [ { "enabled": false, "time": "07:00", "days": "1111100", "chime": "triple_beep", "label": "", "once": false, "snooze_min": 9 }, "... up to 4" ] }
```

Pages: `date`, `temp`, `cond`, `wind`, `hilo`, `feels`, `sun`, `indoor` (temperature, humidity and dew point with trend arrows), `air` (BME680 air-quality score), `baro` (pressure and Zambretti forecast; uses the sensor when one is present, otherwise the `pressure_msl` the weather service reports, with a small `s` or `w` in the corner saying which), `tide` (next high and low water), `moon` (phase, lit fraction, days to full / new), `uv` (ultraviolet index now and today's peak, with the WHO band), `sky` (cloud cover and visibility), `rain` (rainfall today and the chance), `water` (sea temperature at the tide station), `world` (the `time.tz2_*` zone), `event` (days to the nearest countdown in `events`). `display.moon_page` keeps the moon page in the rotation. **`display.pages` names are index-aligned with the firmware's `PageId` enum, so new pages are only ever appended.** Alarm `days` is a 7-character string Monday..Sunday (`1` = on). Severities: `Unknown`, `Minor`, `Moderate`, `Severe`,
`Extreme`. `time.date_order` is `mdy`, `dmy` or `iso`. Sending `"tz2_id"` without `"tz2_posix"` fills the second zone's POSIX string from the same table as `tz_id`; an unknown id clears it. `wifi.dhcp` false needs at least `ip` and `gateway`, and every address field must be a dotted quad; a second network is tried alternately with the first while the clock retries. `webhook.header_value` set to `"***"` keeps the stored secret, like the other secrets. A `webhook` body is `{<title_key>, <body_key>, "event", "device"}` where `event` is `alert`, `lightning`, `alarm`, `air`, `boot` or `test`; only HTTP 200 counts as success and a failure is not retried. An `events` entry with `yearly` false needs a `year` of 2024 or later when enabled. A `display.holidays` entry needs both `month` and `day` to count, and its colour is `#RRGGBB`. Chimes: `none`, `two_tone`, `triple_beep`, `chirp`, `alarm_beeps`, `doorbell`, `arpeggio`, `sonar`, `sos`, `siren_hilo`, `siren_wail`,
`siren_yelp`, `nws_1050`, `eas_attention`, `eas_full`. `audio.chime_extreme` is used for Extreme alerts, `audio.chime` for everything else. `audio.speech` controls the spoken announcements
(the event name of a new alert, "Lightning nearby", "Alarm" / "Timer finished", demo scenario names; `repeat` 1..3); an alert event without
a clip in the voice pack is announced as "Weather alert". `audio.speech.say_time` allows the spoken time, which the
firmware assembles from the pack's number words rather than a clip per minute, so it needs pack version 8 or later
(`/api/status.speech.can_say_time` reports whether the installed pack has them). `audio.hourly_*` control the hourly
chime: `hourly_strike` strikes the hour 1..12 times about a second apart, `hourly_half` adds one chime on the half hour,
`hourly_speak` follows it with the spoken time, and quiet hours silence all of it. `indoor.temp_offset` is in the display unit (F when `weather.units` is imperial); `indoor.altitude_m` -1 uses the
elevation Open-Meteo reports; `pressure_unit` `auto` picks inHg with imperial units. Trend arrows in `/api/status.indoor` are `steady`, `rising`, `falling`,
`rising fast`, `falling fast` (thresholds 0.5 C / 3 % over `trend_min`, 1 hPa over `pressure_trend_min`, fast = three times that). With a BME680 `indoor` also carries `gas_kohm`, `air_score` (0..100), `air_level` (`good`/`fair`/`poor`/`unknown`), `air_baseline_kohm`, `trend_air`;
every humidity sensor adds `dew_point_c`, `abs_humidity` (g/m3), `heat_index_c`, `condensation` (`none`/`possible`/`likely`), `mould_risk` (`none`/`elevated`/`high`) and
`forecast {text, z, trend}` (Zambretti, empty until 30 minutes of pressure history exist). Drivers: `SHIFTREG`, `FM6124`, `FM6126A`,
`ICN2038S`, `MBI5124`, `DP3246`. Sending `"tz_id"` without `"tz_posix"` fills the POSIX string from the built-in
US zone table. `wifi.pass` / `wifi.ap_pass` set to `"***"` keep the stored secret.
