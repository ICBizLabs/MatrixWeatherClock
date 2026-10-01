# Bring-up and testing checklist

Commands assume the repo root. `pio` is PlatformIO Core (`pip install platformio`).

```sh
pio run                      # build
pio run -t upload            # flash over USB (hold BOOT + tap RST if the port is missing)
pio device monitor           # serial log at 115200
```

1. **Boot** – the monitor shows `MWC x.y.z boot`, the I2C map (`es8311=0x18 pca9557=0x.. rtc=0x51`) and
   `HUB75 ok 64x32 @ NNN Hz`. The panel plays the test pattern for 10 s on the very first boot.
2. **WiFi** – without credentials the AP `MatrixWeatherClock-XXXX` appears; connect, open http://4.3.2.1/, enter SSID and
   password on the WiFi tab and save. The panel shows `SETUP WIFI` / `4.3.2.1` until it is online, then the time.
   `ping matrixweatherclock.local` answers.
3. **Config** – `curl -s http://matrixweatherclock.local/api/config | jq .display.brightness`, then
   `curl -X POST http://matrixweatherclock.local/api/config -H 'Content-Type: application/json' -d '{"display":{"brightness":30}}'`
   dims the panel at once and survives a power cycle. `-d '{"location":{"lat":999}}'` returns HTTP 400.
4. **Weather** – `curl -s http://matrixweatherclock.local/api/weather | jq .cur.temp` matches
   `curl -s 'http://api.open-meteo.com/v1/forecast?latitude=LAT&longitude=LON&current=temperature_2m&temperature_unit=fahrenheit'`.
   Bottom pages rotate; the forecast screen appears every third cycle. Unplug the router: the last data stays and
   `/api/status` `net.wx_err` shows the failure.
5. **Alerts** – set the NWS contact on the Location tab, then
   `curl -X POST http://matrixweatherclock.local/api/test/alert -H 'Content-Type: application/json' -d '{"event":"Tornado Warning","severity":"Extreme","headline":"Test until 5 PM","minutes":3}'`
   shows a red scrolling banner with a flashing frame; the chime plays (outside quiet hours); `GET /api/alerts`
   lists it; it disappears after 3 minutes. Injecting the same test twice does not flash again.
   For a live alert run `tools/find_nws_test_point.sh` and point the clock at the printed coordinates.
6. **Audio** – `curl -X POST http://matrixweatherclock.local/api/test/chime -H 'Content-Type: application/json' -d '{"force":true}'`
   plays; with quiet hours covering "now" a test alert stays silent and `/api/log` shows `chime suppressed: quiet hours`.
   **Speech** – with WiFi up, `curl -X POST http://matrixweatherclock.local/api/voice/download`, then watch
   `curl -s http://matrixweatherclock.local/api/status | jq .speech` go `downloading` (with `progress`) → `verifying` → `installed`
   and `/api/log` print `voice: pack v1 installed (134 clips, en_US-ljspeech-medium)`. Then
   `curl -X POST http://matrixweatherclock.local/api/test/say -H 'Content-Type: application/json' -d '{"text":"Tornado Warning"}'`
   speaks (a phrase that is not in the pack returns 404), a test alert plays its chime followed by the event name,
   a 5-second timer (`POST /api/timer {"seconds":5}`) beeps and says "Timer finished" once and then only beeps,
   and `POST '/api/demo?on=1&sound=1'` announces every scenario. Power-cycle: `speech.installed` stays true.
   **Radar** – about 20 s after WiFi `/api/log` shows `radar: 11 frames, echoes N%`; `curl -s http://matrixweatherclock.local/api/radar | jq .`
   lists eleven ages from 50 down to 0 minutes; `curl -X POST http://matrixweatherclock.local/api/show -d screen=radar` plays the
   loop on the panel with the blinking home cross; the Status tab's Radar card plays it enlarged. With no rain within
   100 km the frames are black apart from the marker, which is correct.
   **Indoor sensor** – with a BME280/BME680 wired to SDA 1 / SCL 2 the boot log shows `indoor: BME280 at 0x76`,
   `curl -s http://matrixweatherclock.local/api/status | jq .indoor` has `valid: true` within 10 s, the `indoor` page appears in
   the rotation, and after 10 minutes the arrows start (breathe on the sensor: humidity rises, arrow up). The Status
   tab's Indoor card charts `/api/indoor/history`.
   With a BME680 the log says `gas sensor on`; after five minutes `/api/status | jq .indoor.air_score` shows a score
   and the `air` page appears; breathing on the sensor or opening a marker pen drops the score and, after two minutes
   below the poor threshold, triggers the chime, "Air quality poor" and a push. The `baro` page scrolls the Zambretti
   text once 30 minutes of pressure history exist.
   **Remote** – with a receiver on GPIO 44 the boot log shows `remote: IR receiver on GPIO 44`; pressing any key
   raises `received` in `curl -s http://matrixweatherclock.local/api/remote` and logs `remote: code 0x... not mapped`; after
   mapping it on the Remote tab the key runs its action. `curl -X POST 'http://matrixweatherclock.local/api/action?name=show_radar'`
   does the same from a script, and http://matrixweatherclock.local/remote works on a phone.
7. **RTC / buttons / OTA** – power-cycle with WiFi unavailable: the time is right immediately and
   `/api/status` `time.source` is `rtc`. K1 short = next page, K1 long = test pattern, K2 short = acknowledge alerts,
   K2 long = test chime, K3 short = refresh data, K3 long = reboot.
   `curl -F 'firmware=@.pio/build/seengreat_hub75_s3/firmware.bin' http://matrixweatherclock.local/update` reboots into the
   new version with settings intact.
8. **0.14.0 additions** – verified on the device at 192.168.4.56 on 2026-09-27, except where noted.
   Results: config flags, new pages, weather values, water temperature, webhook, stopwatch and sleep timer all pass.
   The voice pack download wedged the clock once and is the open item; see the end of this section.
   **Config flag width** – save each of the fifteen existing sections from the web UI and confirm the log line
   `config: applying changes 0x0.....` names the expected bit and the change actually takes effect; the panel section
   must still answer `reboot_required`. Then save the two new sections (`webhook`, `events`) and confirm the same.
   This is the check that the `uint16_t` to `uint32_t` widening did not break the existing bits.
   **New pages** – add `uv, sky, rain, water, world, event` to `display.pages`. Each must either draw or hide itself:
   `curl -s http://matrixweatherclock.local/api/weather | jq .cur` should carry `uv`, `cloud`, `visibility` and `precip`,
   and `.daily[0]` should carry `rain_sum` and `uv_max`. Check the ultraviolet page's band colour against the number,
   and that visibility reads in miles with imperial units (Open-Meteo returns feet on that unit set, kilometres
   otherwise, which is the conversion most likely to be wrong).
   **Water temperature** – with a station that reports one, `/api/status | jq .tide.water_temp` appears within a
   refresh and the `water` page shows it. A station without a thermometer must leave the field absent and the page
   hidden, not show a wrong number.
   **World clock** – set up to four zones on the Display tab, confirm the full screen shows them all and the `world`
   page shows the first, and that the main clock and the tide times are still right afterwards. Since 0.16.0 the zone
   conversion no longer borrows the process timezone (see `src/time/posix_tz.h`), so that last check should be dull --
   but it is the one that would catch the regression if anything ever swaps TZ per lookup again.
   **Countdowns** – set a one-off and a yearly date, confirm the `event` page counts down, says `TOMORROW` the day
   before and `TODAY` on the day, and that a yearly date rolls to next year once it has passed.
   **Hourly chime** – set the clock a minute before the hour with `hourly_chime` and `hourly_strike` on: the hour
   should strike the right number of times about a second apart, then speak the time if `hourly_speak` is on. Inside
   quiet hours nothing should sound. While an alarm rings or is snoozed, nothing should sound.
   **Spoken time** – `curl -X POST 'http://matrixweatherclock.local/api/test/say-time?force=1'` should say the time as one
   utterance with no gaps between the words. It needs voice pack v8; on an older pack it must answer 409 rather than
   speak a partial phrase. Check both 12- and 24-hour settings, and minutes 0, 5, 15, 30 and 47.
   **Stopwatch** – `POST /api/stopwatch {"action":"start"}`, confirm it counts up on the panel, pause and confirm it
   holds, reset and confirm it leaves the display.
   **Sleep timer** – `POST /api/sleep {"minutes":1}`, confirm the panel fades to black over `sleep_fade_sec` and stays
   dark, then `{"minutes":0}` restores the scheduled brightness.
   **One-off alarm** – set an alarm one minute out with `once` on, let it ring, then confirm `/api/config` shows it
   disabled and that it stays disabled after a reboot.
   **Webhook** – point it at a request bin or `ntfy.sh`, press *Send a test*, and confirm the body carries the two
   configured field names plus `event` and `device`. Inject a test alert and confirm it fires for alerts too. Point it
   at a dead host and confirm `/api/status.webhook.errors` counts up without disturbing anything else.
   **Two networks and static address** – save a second SSID, take the first network down, and confirm the log shows
   `wifi: retrying STA on <second>` and the clock associates. Then set a static address, save, and confirm the clock
   comes back on that address with working DNS. Getting this wrong takes the device off the network, so have the
   serial console or the setup access point as a way back.
   **Memory** – with radar on and the web UI open, `curl -s http://matrixweatherclock.local/api/status | jq '.sys'` must
   keep `heap_largest` above 18432 and `heap_free` above 47104, which is what the TLS guard needs. Static internal RAM
   grew from 67,560 to 70,696 bytes in this release, so confirm HTTPS still works: watch a tide fetch and an update
   check succeed rather than logging `low memory for TLS`.

   **Results, 2026-09-27, firmware 0.14.0 on the device.**
   - Flag widening: all seventeen sections apply. The log shows `applying changes 0x030000` for `webhook` and
     `events`, which is the proof that the two bits above 16 survive; under the old `uint16_t` they were zero. The
     panel section still answers `reboot_required`. Note the OTA upload needs `curl -H "Expect:"`; without it the
     async server leaves the updater engaged and the next attempt fails with `begin failed`, needing a reboot.
   - Memory: steady state is 101 KB free internal heap and a 58 KB largest block, against the TLS guard's 46 KB and
     18 KB. Right after an OTA the largest block dips to 34 KB, still comfortably above the guard.
   - Weather: `uv` 6.9, `cloud_cover` 0%, `visibility` 25.8 miles, `rain_sum` and `uv_max` all present. Visibility
     was cross-checked against the raw feed: 136,154 ft / 5280 = 25.8 miles, so the imperial unit really is feet.
   - Pages: all six draw correctly. `uv` showed "UV 7 / HIGH" in the orange 6-8 band, `sky` "CLOUD 0% / VIS 20 MI",
     `rain` "RAIN 0.00\" / CHANCE 0%", `event` "CHRISTMAS / 89 DAYS" (correct for 27 Sep to 25 Dec), `world` 7:01 PM
     UTC beside a local 3:01 PM Eastern, which also confirms the timezone swap leaves the main clock alone.
   - Water temperature: station 8727012 does not offer the product, and the page correctly showed "NO WATER TEMP"
     rather than a wrong number. Pointing at 8726724 (Clearwater Beach) gave 82.0 F, matching NOAA exactly.
   - Webhook: both the test and an injected alert arrived at a listener with the configured field names, the
     `event` and `device` fields, and the custom `X-Clock-Token` header.
   - Stopwatch and sleep timer: counted 7.0 s on the panel matching 7,076 ms from the API; sleep armed, reported
     120 s remaining and cancelled. Bad inputs rejected with 400, unconfigured webhook and pack-less spoken time
     with 409.
   - **Open: the voice pack.** Fetching pack v8 (4.4 MB) from a local HTTP server reached 35% and the clock then
     went off the network and did not return until it was power-cycled. It recorded no crash (reset reason
     power-on, black box clean), so it hung rather than panicking; a watchdog would have shown. The old pack
     survived intact and `speech.state` stayed `installed` with no error, so the swap logic is safe. Spoken time
     and `hourly_speak` therefore remain unverified. Before retrying, note the pack grew from 3.98 to 4.4 MB, so
     old plus new now occupy 8.4 MB of the 9.9 MB filesystem during the swap; that is within the free-space check
     but tighter than before. Retry with nothing else polling the device, and watch the serial console.

9. **0.15.0 additions** – verified on the device at 192.168.4.56 on 2026-09-27.
   **Page picker** – the Display tab lists all eighteen pages with a tick box, a drag handle and up/down arrows;
   `GET /api/config.pages_available` feeds it so it can never drift from the `PageId` enum. Checked in a browser:
   eighteen rows render, ticking a page adds it in the position it sits in, the arrows and drag both reorder,
   position numbers renumber over the ticked rows only, the first up and last down arrows disable, and Save posts
   the list in the displayed order. A page ticked while it sits below the ticked block lands last, and one dragged
   to the top before ticking stays first.
   **Pressure** – no longer on the indoor page, which now shows temperature, humidity and dew point (81 F at 43 %
   gave a dew point of 57 F, correct). The barometer page prefers the sensor: with the BME688 connected it read
   29.93 inHg with an `s` marker, matching the sensor's 1013.5 hPa sea-level figure. With `indoor.enabled` turned
   off it fell back to 29.90 inHg with a `w` marker, matching the 1012.6 hPa Open-Meteo reports. Both sources agree
   to about a tenth of a hPa, which is the cross-check worth repeating after any change here.
   The reported tendency needs history: `press_span_min` is 0 until about 20 minutes of fetches have accumulated,
   and the page says LEARNING TREND until 30 minutes, so the Zambretti text on a sensorless clock appears only
   after roughly half an hour. Worth re-checking a few hours after a reboot that `d_press_3h` becomes non-zero.

10. **0.16.0 additions** – verified on the device at 192.168.4.77 on 2026-09-29.
    **Zone table** – grew from ten US zones to 78 worldwide. `tools/check_timezones.py` is the standing check and
    must keep passing: it parses `src/time/tz_table.cpp`, reads the system tzfiles, and compares both the C library's
    POSIX parser and the firmware's own evaluator against the real IANA data at every instant from today through six
    years out, plus every real transition to the minute. It caught two rules of mine that were genuinely wrong
    (British Columbia and Alberta both went to permanent offsets during 2026, so `America/Vancouver` is `MST7` and
    `America/Edmonton` is `CST6` while Toronto and Winnipeg still switch). Its horizon starts at today on purpose: a
    single POSIX rule cannot also describe a zone's past, so verifying from today forward is the honest test.
    Current result: 78 zones, 844,464 instants, 0 disagreements, plus 17 edge cases under UBSan and ASan.
    **World clock screen** – four rows, label left and time right-aligned, with a `+1`/`-1` marker when that zone is
    on another date. Verified by rebuilding the expected frame from the IANA data with the same TomThumb font and
    comparing `GET /api/frame` pixel for pixel: exact matches for Singapore, Kyiv, UTC and Iran (`IRST-3:30`, a
    half-hour offset), and for Hawaii showing `7:38P-1` while home was the next day. Right-aligned text ends at x=62,
    not 63 -- `drawTextRight(s, W-1, ..)` draws at `W-1-textWidth`, the same one-pixel margin `drawForecast` uses.
    **The heap leak, which is the thing to remember.** `timesvc::zoneNow` used to borrow the process timezone:
    setenv, tzset, localtime_r, then put it back. On this newlib that leaks about 50 bytes per call. It was survivable
    when only the old world *page* called it, but refreshing four rows twice a second meant 8 calls a second, and the
    device lost **24 KB a minute** and panicked in `render:clock` with 600 bytes of heap left, every four minutes. It
    was diagnosed by clearing the zones, which made the heap perfectly flat, and fixed by evaluating the POSIX rule
    arithmetically in `src/time/posix_tz.cpp` -- no allocation, no global state, no lock. After the fix, four zones
    configured: heap flat at ~96 KB free with `heap_largest` pinned at 51 KB over several minutes. If anything ever
    calls setenv/tzset per lookup again, this is what it will look like.
    **`GET /api/timezones`** – the same lesson in miniature. Built as an `AsyncJsonResponse` it cost ~40 KB of heap
    for 5.3 KB of text, because ArduinoJson copies every string, and it did not hand it straight back: one call took
    `heap_largest` from 56 KB to 31 KB. It is now generated a chunk at a time straight out of flash. Ten consecutive
    calls: identical 5,334-byte payloads, `heap_largest` unchanged at 34,804.
    **Legacy migration** – a config carrying only the pre-0.16.0 `time.tz2_*` keys puts them in `world.zones[0]` and
    reports `applied:["world"]`; confirmed on the device with the rows cleared first.
    **On a schedule** – watched for 22 minutes without prompting it: the four full screens took turns in clean
    round-robin order, radar then world then forecast then hourly, one each about five minutes apart, and the world
    clock stayed up for its `show_sec`. Worth knowing before testing this: with `page_sec` 6 and
    `forecast_every_n_cycles` 3 an opportunity only comes round every few minutes, and there are four candidates, so
    a five-minute look is not long enough to conclude anything.
    **On demand** – `POST /api/show?screen=world`, `POST /api/action?name=show_world` and the phone remote button all
    work. The world branch sits above the `!wx.valid` gate in `requestFullScreen` on purpose: a world clock needs no
    weather. It also refreshes the cache itself when asked while empty, so the first request right after saving zones
    is not refused.
    **Change flags** – every section posted its own current values straight back and each was listed in `applied`,
    which is the check that the widened `uint32_t` flag word and the new `CHG_WORLD` bit did not shift anything.
    **`tm_isdst`** – `posix_tz::zone_tm` sets it (New York September 1, January 0, Sydney the other way round,
    Arizona always 0). Nothing reads it today, but returning 0 all year would quietly break whatever does first.

11. **0.17.0 additions** – verified on the device at 192.168.4.77 on 2026-10-01.
    **The structural fix first.** A theme used to carry only three of the six colours, and `temp`, `hi` and `lo` were
    read straight from config at ten sites, so on Christmas the clock turned red and the date green while the forecast
    strip and the high/low page stayed on the everyday orange and blue. `themes::Theme` now carries all six and
    `renderer.cpp` has `colTemp()`/`colHi()`/`colLo()` beside the existing three; every drawing site goes through an
    accessor. **If you add a colour, add an accessor** -- reading `g_cfg.display.colors` directly is how half the
    panel stops following a theme.
    Four sites also hardcoded a copy of a config default, so the setting had no effect there: the sunrise and sunset
    lines, the rainfall line and the sea temperature. They read their fields now. The date page's number line used the
    *text* colour, so "Date" only painted the weekday; the whole page takes the date colour now.
    **Palettes** – eight, applied by `POST /api/config {"display":{"color_preset":"ocean"}}`, which is write-only and
    copies the six colours into `display.colors`. Verified by parsing `src/display/themes.cpp` for the expected values
    and comparing against both `/api/config` and the actual pixels in `/api/frame`: all eight matched exactly, the
    clock digits included. Also checked that a colour tweaked by hand after applying a palette survives a later save,
    which is the property that makes "the pickers are the truth" true.
    **Holiday control** – `display.holidays_enabled` is a bitmask by table position, so `themes::HOLIDAYS` is
    **append-only**; inserting or reordering would silently remap which holidays somebody had switched off. All nine
    were forced in turn and the forecast strip was confirmed themed, which is the thing that did not work before.
    Forcing a holiday that is unticked still works, by design. An unknown id in `force_theme` or `color_preset` is
    rejected with HTTP 400, and the renderer falls back to automatic if one ever goes stale.
    **A trap worth knowing when testing this.** A pinned theme outranks `display.colors`, so if `force_theme` is set,
    applying a palette writes the config and changes nothing on the panel. My first verification run failed eight
    palette checks for exactly this reason -- the clock was pinned to New Year's Eve, and every "wrong" colour decoded
    to that theme's gold. Clear `force_theme` before testing colours. The Colours card now says so on screen when a
    theme is pinned.
    The other first-run failures were the test grabbing `/api/frame` after the forecast screen had already timed out;
    assert the screen is actually up (`sys.screen`) before reading pixels.
