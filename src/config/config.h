#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// ---- enumerations shared across modules -------------------------------------------------------
enum class Severity : uint8_t { Unknown = 0, Minor, Moderate, Severe, Extreme };
const char* severity_name(Severity s);
bool        severity_parse(const char* s, Severity& out);

enum PageId : uint8_t { PAGE_DATE = 0, PAGE_TEMP, PAGE_COND, PAGE_WIND, PAGE_HILO, PAGE_FEELS, PAGE_SUN, PAGE_INDOOR, PAGE_AIR, PAGE_BARO, PAGE_COUNT };
const char* page_name(uint8_t id);
bool        page_parse(const char* s, uint8_t& out);

enum class ChimeStyle : uint8_t {
  None = 0, TwoTone, TripleBeep, Chirp,
  EasAttention,   // Emergency Alert System attention signal: 853 + 960 Hz dual tone, 8 s
  EasFull,        // SAME-style data bursts, attention signal, end-of-message bursts (~19 s)
  Nws1050,        // NOAA Weather Radio 1050 Hz warning tone, 5 s
  SirenWail, SirenYelp, SirenHiLo,
  AlarmBeeps, Doorbell, Sos, Arpeggio, Sonar,
  COUNT
};
const char* chime_name(ChimeStyle c);
bool        chime_parse(const char* s, ChimeStyle& out);

constexpr uint8_t PANEL_DRIVER_COUNT = 6;         // SHIFTREG, FM6124, FM6126A, ICN2038S, MBI5124, DP3246
const char* panel_driver_name(uint8_t d);
bool        panel_driver_parse(const char* s, uint8_t& out);

// ---- configuration sections (defaults are the in-class initialisers) ---------------------------
struct WifiConfig {
  char ssid[33] = "";
  char pass[65] = "";
  char hostname[32] = "matrixweatherclock";
  char ap_pass[65] = "";        // empty = open access point
  uint8_t tx_power = 34;        // wifi_power_t value: 34 = 8.5 dBm, 78 = 19.5 dBm
};
struct LocationConfig {
  float lat = 39.7392f;
  float lon = -104.9903f;
  char name[48] = "";
};
struct TimeConfig {
  char tz_id[40] = "America/Denver";
  char tz_posix[64] = "MST7MDT,M3.2.0,M11.1.0";
  char ntp1[48] = "pool.ntp.org";
  char ntp2[48] = "time.nist.gov";
  bool use_24h = false;
  bool show_seconds = false;
};
struct WeatherConfig {
  bool enabled = true;
  bool imperial = true;         // false = metric
  uint16_t refresh_min = 15;
  uint8_t forecast_days = 3;
};
struct AlertsConfig {
  bool enabled = true;
  uint16_t poll_sec = 120;
  char user_agent_contact[64] = "";   // NWS requires a contact in the User-Agent
  Severity min_severity = Severity::Moderate;
  char ignored_events[192] = "";      // comma separated event names, case-insensitive
  Severity chime_min_severity = Severity::Severe;
  uint16_t flash_frame_sec = 60;
  uint8_t banner_px_per_s = 20;
  bool tls_verify = false;
};
struct ScheduleConfig {
  bool enabled = true;
  bool follow_sun = false;      // use sunrise/sunset from the weather data instead of the fixed times
  int16_t sun_offset_min = 30;  // day level starts this many minutes before sunrise and ends after sunset
  uint16_t day_start = 7 * 60;
  uint8_t day_level = 160;
  uint16_t night_start = 21 * 60;
  uint8_t night_level = 40;
};
struct NightModeConfig {
  bool enabled = true;
  uint16_t start = 23 * 60;
  uint16_t end = 6 * 60;
  uint8_t level = 8;
  bool hide_bottom = true;
};
struct ColorsConfig {            // RGB888
  uint32_t time = 0xFFFFFF;
  uint32_t date = 0x80C0FF;
  uint32_t temp = 0xFFD060;
  uint32_t text = 0xC0C0C0;
  uint32_t hi = 0xFF8060;
  uint32_t lo = 0x60A0FF;
};
struct DisplayConfig {
  uint8_t brightness = 128;
  float gamma = 2.2f;
  uint8_t page_sec = 6;
  uint8_t pages[PAGE_COUNT] = { PAGE_DATE, PAGE_TEMP, PAGE_COND, PAGE_WIND };
  uint8_t npages = 4;
  bool forecast_page = true;
  uint8_t forecast_every_n_cycles = 3;
  bool colon_blink = true;
  uint8_t ip_on_connect_sec = 20;   // show the IP address this long after WiFi connects, 0 = never
  bool hourly_page = true;          // 12-hour temperature / rain-chance graph screen, alternates with the forecast
  bool transitions = true;          // slide pages in from the right
  bool precip_fx = true;            // animated rain / snow / lightning behind the weather pages
  bool holiday_themes = true;       // holiday colours and effects on special dates
  ScheduleConfig schedule;
  NightModeConfig night;
  ColorsConfig colors;
};
struct PanelConfig {
  uint16_t width = 64;
  uint16_t height = 32;
  uint8_t chain = 1;
  uint8_t driver = 0;           // index into panel_driver_name()
  bool clkphase = false;        // P4-256x128-2121-A5 panels need the negative clock edge (column 0 lost otherwise)
  uint8_t latch_blanking = 2;
  uint32_t i2s_speed_hz = 8000000;
  uint8_t min_refresh_hz = 120;
  uint8_t max_brightness = 255;
  uint8_t color_depth_bits = 8;
  bool double_buffer = false;
  bool swap_rb = false;
};
struct QuietConfig {
  bool enabled = true;
  uint16_t start = 22 * 60;
  uint16_t end = 7 * 60;
};
struct SpeechConfig {         // spoken announcements from the downloaded voice pack (after the chime)
  bool enabled = true;
  bool alerts = true;           // NWS event name ("Tornado Warning")
  bool lightning = true;        // "Lightning nearby"
  bool alarms = true;           // "Alarm" / "Timer finished" once when the ring starts
  bool demo = true;             // scenario names in demo mode (with demo sounds)
  bool indoor = true;           // "Air quality poor"
  uint8_t repeat = 1;           // say each announcement 1..3 times
};
struct AudioConfig {
  bool enabled = true;
  uint8_t volume = 60;          // percent
  ChimeStyle chime = ChimeStyle::TwoTone;              // alerts, lightning, messages, pushes
  ChimeStyle chime_extreme = ChimeStyle::EasAttention; // Extreme alerts (tornado, hurricane, ...)
  uint16_t repeat_min = 0;      // re-chime interval while an unacknowledged alert stands, 0 = once
  QuietConfig quiet;
  SpeechConfig speech;
};
constexpr uint8_t MAX_ALARMS = 4;
struct AlarmConfig {
  bool enabled = false;
  uint16_t minute = 7 * 60;     // minute of day
  uint8_t days = 0x1F;          // bit 0 = Monday ... bit 6 = Sunday
  ChimeStyle chime = ChimeStyle::TripleBeep;
  char label[16] = "";
};
struct AlarmsConfig { AlarmConfig items[MAX_ALARMS]; };

struct LightningConfig {          // real-time strikes from the Blitzortung network via the community MQTT relay
  bool enabled = false;
  char server[64] = "blitzortung.ha.sed.pl";
  uint16_t port = 1883;
  uint16_t radius_km = 40;
  uint16_t window_min = 15;       // a strike counts as "nearby" for this long
  bool chime = true;              // chime on the first strike of a storm (then at most every 5 minutes)
  bool show_bolt = true;          // bolt icon next to the clock while strikes are nearby
};

struct PushbulletConfig {         // phone notifications out, pushes in (https://www.pushbullet.com/#settings/account)
  char token[40] = "";            // access token; empty = disabled
  char device_iden[24] = "";      // filled in automatically when the clock registers itself as a device
  bool notify_alerts = true;
  Severity notify_min_severity = Severity::Severe;
  bool notify_lightning = true;
  bool notify_alarms = false;
  bool notify_air = true;         // poor indoor air quality (BME680)
  bool show_pushes = true;        // pushes sent to the account (or to this device) scroll on the panel
  uint16_t poll_sec = 60;
  uint16_t show_sec = 60;
  bool chime = true;              // chime when a push is shown
};

struct UpdateConfig {             // self-update from the web installer's manifest (GitHub Pages)
  bool check = true;              // look for new versions periodically
  bool auto_install = true;       // install automatically when a newer version is found
  char url[128] = "https://icbizlabs.github.io/MatrixWeatherClock/manifest.json";
  uint16_t check_hours = 6;
};

struct IndoorConfig {             // BME280 / BMP280 / BME680 on the I2C header (SDA 1, SCL 2)
  bool enabled = true;
  bool auto_page = true;          // add the "indoor" page to the rotation when a sensor is found
  uint16_t sample_sec = 10;
  float temp_offset = 0;          // calibration, in the display unit (F when imperial); the board runs warm
  float humidity_offset = 0;      // % RH
  float altitude_m = -1;          // for sea-level pressure; -1 = elevation reported by the weather service
  bool sea_level = true;          // show pressure reduced to sea level when an altitude is known
  uint8_t pressure_unit = 0;      // 0 = auto (inHg when imperial, else hPa), 1 = hPa, 2 = inHg
  uint16_t trend_min = 60;        // window for the temperature / humidity trend arrows
  uint16_t pressure_trend_min = 180;   // window for the pressure tendency (WMO uses 3 hours)
  bool gas = true;                // BME680: run the gas sensor heater (adds about 1 C of self-heating)
  uint8_t air_fair_below = 80;    // air-quality score thresholds: good >= fair_below, fair >= poor_below, else poor
  uint8_t air_poor_below = 60;
  bool air_alert = true;          // chime / speech / push when the air stays poor
  uint16_t air_alert_min = 60;    // at most one air alert this often
  float temp_offset_c = 0;        // derived: temp_offset converted to C
};

struct RadarConfig {              // animated NEXRAD loop from the Iowa Environmental Mesonet WMS
  bool enabled = true;
  uint16_t radius_km = 100;       // half the width of the view (the panel shows 2 x radius wide, radius tall)
  uint8_t every_n_cycles = 4;     // takes a turn among the full screens this often
  bool show_when_precip = true;   // while echoes are near or it is raining / snowing: every precip_every_n_cycles
  uint8_t precip_every_n_cycles = 2;
  uint16_t frame_ms = 350;        // animation speed
  uint16_t hold_ms = 1500;        // pause on the newest frame
  uint8_t show_sec = 12;          // how long the radar screen stays
  uint8_t refresh_min = 5;        // new composite every 5 minutes
  uint8_t base_map = 3;           // underlay from NASA GIBS: bit 0 = coastline, bit 1 = land / water tint (3 = both, 0 = none)
  uint8_t source = 0;             // 0 = NOAA nowCOAST MRMS mosaic (quality controlled), 1 = Iowa Mesonet NEXRAD n0r composite (unfiltered)
};

constexpr uint8_t MAX_REMOTE_BUTTONS = 24;
struct RemoteButton { uint32_t code = 0; uint8_t action = 0; };   // action = actions::Id
struct RemoteConfig {             // infrared receiver module on one GPIO (see io/ir_remote.h)
  bool enabled = true;
  int8_t pin = 44;                // RX0 pad on the bottom header; the console runs over USB so it is free
  RemoteButton buttons[MAX_REMOTE_BUTTONS];
  uint8_t nbuttons = 0;
};

struct AppConfig {
  WifiConfig wifi;
  LocationConfig location;
  TimeConfig time;
  WeatherConfig weather;
  AlertsConfig alerts;
  DisplayConfig display;
  PanelConfig panel;
  AudioConfig audio;
  AlarmsConfig alarms;
  LightningConfig lightning;
  PushbulletConfig pushbullet;
  UpdateConfig update;
  IndoorConfig indoor;
  RadarConfig radar;
  RemoteConfig remote;
  bool first_boot = true;
};

// Bit flags telling which sections a JSON merge touched (used to apply changes live / ask for a reboot)
enum : uint16_t {
  CHG_WIFI = 1, CHG_LOCATION = 2, CHG_TIME = 4, CHG_WEATHER = 8,
  CHG_ALERTS = 16, CHG_DISPLAY = 32, CHG_PANEL = 64, CHG_AUDIO = 128, CHG_ALARMS = 256, CHG_LIGHTNING = 512, CHG_PUSHBULLET = 1024, CHG_UPDATE = 2048, CHG_INDOOR = 4096, CHG_RADAR = 8192, CHG_REMOTE = 16384
};

extern AppConfig g_cfg;

bool config_load();                                   // LittleFS /config.json -> g_cfg (defaults when missing/invalid)
bool config_save(const AppConfig& c);                 // write /config.tmp then rename
bool config_from_json(JsonObjectConst src, AppConfig& c, uint16_t& changed, String& err);  // partial merge + validation
void config_to_json(const AppConfig& c, JsonObject dst, bool mask_secrets);
void config_factory_reset();                          // removes /config.json and reboots
