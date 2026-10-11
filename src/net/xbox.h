#pragma once
#include <Arduino.h>
#include <time.h>
#include "config/config.h"

// Xbox player stats through OpenXBL (https://xbl.io), a free proxy for the Xbox Live web API that only needs an API
// key. Follows one gamertag: gamerscore, online / now playing, the latest achievement and how many friends are
// online. Presence and friends refresh every 5 minutes, gamerscore and achievements every 15, about 40 requests an
// hour against the free tier's 150.
namespace xbox {
  struct Data {
    bool valid = false;
    char gamertag[16] = "";          // as Xbox spells it
    uint32_t gamerscore = 0;
    bool online = false;
    char game[40] = "";              // the title in the foreground while online, "" = none or the home screen
    int16_t friends_online = -1;     // -1 = not known (private list or switched off)
    char friend_names[3][16] = {};
    char achievement[48] = "";       // newest unlocked achievement
    char achievement_game[40] = "";
    time_t achievement_t = 0;
  };
  struct Status {
    bool enabled;
    bool valid;
    char xuid[24];
    uint32_t last_ok_ms, last_err_ms;
    uint8_t fails;
    uint16_t requests_hour;          // requests sent in the last hour
    char err[48];
  };
  void begin();
  void applyConfig();                          // CHG_XBOX: forget and look the gamertag up again
  void requestRefresh();
  bool due(const AppConfig& cfg, uint32_t now_ms);
  void run(const AppConfig& cfg);              // network task
  Status status();
  bool get(Data& out);                         // copy of the current data; returns out.valid
}
