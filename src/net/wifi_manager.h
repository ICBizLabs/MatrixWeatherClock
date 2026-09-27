#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <IPAddress.h>
#include "config/config.h"

// Station connection with access-point fallback and captive portal.
namespace wifi_mgr {
  void begin(const WifiConfig& wc);
  void loop();
  void applyCredentials(const WifiConfig& wc);   // new SSID/password from the UI
  bool isConnected();
  const char* activeSsid();          // which of the two saved networks is being used
  bool staticIp();
  bool apActive();
  String apSsid();
  IPAddress ip();
  int8_t rssi();
  bool consumeConnectedEvent();                  // true once after each (re)connection
  void startScan();
  int scanToJson(JsonArray out);                 // -1 while scanning, else number of networks
  bool isCaptiveRequest(const String& host);     // AP mode and Host header is not our own address
}
