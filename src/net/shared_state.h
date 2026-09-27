#pragma once
#include <Arduino.h>

// Data produced by the network task and consumed by the renderer / web API. Always copied under a mutex.
struct WeatherCurrent {
  float temp = 0, feels = 0, humidity = 0, wind = 0, gust = 0;
  int16_t wind_dir = 0;
  uint8_t wmo = 0;
  bool is_day = true;
  float uv = -1;            // ultraviolet index, -1 = unknown
  float cloud = -1;         // cloud cover, percent
  float vis = -1;           // visibility, in the display distance unit (miles when imperial, else km)
  float rain = -1;          // precipitation so far this hour, display unit (inch / mm)
};
struct WeatherDaily {
  char date[11] = "";
  uint8_t wmo = 0;
  float tmax = 0, tmin = 0;
  uint8_t pop = 0;          // precipitation probability, percent
  float rain_sum = -1;      // total precipitation for the day, display unit; -1 = unknown
  float uv_max = -1;        // peak ultraviolet index for the day
};
constexpr uint8_t HOURLY_COUNT = 12;
struct WeatherHourly {
  int8_t hour = 0;          // local hour of day
  float temp = 0;
  uint8_t pop = 0;          // precipitation probability, percent
};
struct WeatherData {
  bool valid = false;
  bool imperial = true;
  uint32_t fetched_ms = 0;
  int32_t utc_offset_s = 0;
  WeatherCurrent cur;
  WeatherDaily daily[3];
  uint8_t ndaily = 0;
  WeatherHourly hourly[HOURLY_COUNT];
  uint8_t nhourly = 0;
  int16_t sunrise_min = -1;  // today's sunrise / sunset as minute of local day, -1 = unknown
  int16_t sunset_min = -1;
  float elevation_m = -9999;  // grid-cell elevation reported by Open-Meteo, -9999 = unknown (used for sea-level pressure)
};
struct NetStatus {
  uint32_t last_wx_ok = 0, last_wx_err = 0, last_al_ok = 0, last_al_err = 0;   // millis(), 0 = never
  uint32_t next_wx = 0, next_al = 0;
  uint8_t wx_fails = 0, al_fails = 0;
  char wx_err[48] = "";
  char al_err[48] = "";
};

namespace shared {
  void begin();
  void setWeather(const WeatherData& w);
  bool getWeather(WeatherData& out);        // returns out.valid
  void getNet(NetStatus& out);
  void updateNet(void (*fn)(NetStatus&, void*), void* arg);
}
