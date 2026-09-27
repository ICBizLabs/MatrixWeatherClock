#include "lightning.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <math.h>
#include "app.h"
#include "config/config.h"
#include "wifi_manager.h"
#include "util/log.h"

namespace lightning {
  namespace {
    constexpr size_t MAX_STRIKES = 64;
    constexpr uint32_t CHIME_INTERVAL_MS = 5 * 60000UL;
    constexpr uint8_t GEOHASH_PRECISION = 3;      // ~156 km cells; nine cells cover any radius up to ~150 km
    struct Strike { time_t t; float km; int16_t bearing; };

    WiFiClient net;
    PubSubClient mqtt(net);
    SemaphoreHandle_t mtx = nullptr;
    Strike strikes[MAX_STRIKES];
    size_t head = 0, count = 0;
    uint64_t recentTimes[16]; uint8_t recentIdx = 0;
    volatile bool reconfigure = true;
    volatile bool strikeEvent = false, chimeEvent = false, notifyEvent = false, webhookEvent = false;
    uint32_t lastChime = 0;
    bool wasActive = false;
    uint32_t total = 0;
    LightningConfig cfg;
    float lat = 0, lon = 0;
    String topics[9]; uint8_t ntopics = 0;

    bool take() { return mtx && xSemaphoreTake(mtx, pdMS_TO_TICKS(100)) == pdTRUE; }
    void give() { xSemaphoreGive(mtx); }

    // geohash of a point at the given precision, written as MQTT topic levels: "s/m/u"
    String geohashTopic(double la, double lo, uint8_t precision) {
      static const char* B32 = "0123456789bcdefghjkmnpqrstuvwxyz";
      double latLo = -90, latHi = 90, lonLo = -180, lonHi = 180;
      String out;
      int bit = 0, ch = 0;
      bool even = true;
      while (out.length() < precision * 2 - 1) {
        if (even) { double mid = (lonLo + lonHi) / 2; if (lo >= mid) { ch |= (16 >> bit); lonLo = mid; } else lonHi = mid; }
        else      { double mid = (latLo + latHi) / 2; if (la >= mid) { ch |= (16 >> bit); latLo = mid; } else latHi = mid; }
        even = !even;
        if (++bit == 5) { if (out.length()) out += '/'; out += B32[ch]; bit = 0; ch = 0; }
      }
      return out;
    }

    void buildTopics() {
      ntopics = 0;
      const double cell = 180.0 / 128.0 * 1.0;   // precision-3 cell: 1.40625 degrees in both directions
      for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
        double la = constrain(lat + dy * cell, -89.9, 89.9), lo = lon + dx * cell;
        if (lo > 180) lo -= 360;
        if (lo < -180) lo += 360;
        String t = "blitzortung/1.1/" + geohashTopic(la, lo, GEOHASH_PRECISION) + "/#";
        bool dup = false;
        for (uint8_t i = 0; i < ntopics; i++) if (topics[i] == t) dup = true;
        if (!dup) topics[ntopics++] = t;
      }
    }

    float haversineKm(double la1, double lo1, double la2, double lo2) {
      const double R = 6371.0, d2r = M_PI / 180.0;
      double dLat = (la2 - la1) * d2r, dLon = (lo2 - lo1) * d2r;
      double a = sin(dLat / 2) * sin(dLat / 2) + cos(la1 * d2r) * cos(la2 * d2r) * sin(dLon / 2) * sin(dLon / 2);
      return (float)(2 * R * atan2(sqrt(a), sqrt(1 - a)));
    }
    int16_t bearingDeg(double la1, double lo1, double la2, double lo2) {
      const double d2r = M_PI / 180.0;
      double dLon = (lo2 - lo1) * d2r;
      double y = sin(dLon) * cos(la2 * d2r);
      double x = cos(la1 * d2r) * sin(la2 * d2r) - sin(la1 * d2r) * cos(la2 * d2r) * cos(dLon);
      double b = atan2(y, x) / d2r;
      if (b < 0) b += 360;
      return (int16_t)b;
    }

    void onMessage(char* topic, uint8_t* payload, unsigned int len) {
      JsonDocument doc;
      if (deserializeJson(doc, payload, len)) return;
      double sla = doc["lat"] | 999.0, slo = doc["lon"] | 999.0;
      uint64_t t = doc["time"] | (uint64_t)0;
      if (sla > 90 || slo > 180 || t == 0) return;
      for (uint8_t i = 0; i < 16; i++) if (recentTimes[i] == t) return;   // same strike reported by another region
      recentTimes[recentIdx++ & 15] = t;
      float km = haversineKm(lat, lon, sla, slo);
      if (km > cfg.radius_km) return;
      Strike s = { (time_t)(t / 1000000000ULL), km, bearingDeg(lat, lon, sla, slo) };
      if (take()) {
        strikes[head] = s;
        head = (head + 1) % MAX_STRIKES;
        if (count < MAX_STRIKES) count++;
        total++;
        give();
      }
      strikeEvent = true;
      uint32_t now = millis();
      if (!wasActive || now - lastChime >= CHIME_INTERVAL_MS) { notifyEvent = true; webhookEvent = true; if (cfg.chime) chimeEvent = true; lastChime = now; }
      wasActive = true;
      LOGI("lightning: strike %.1f km %d deg", km, s.bearing);
    }

    void task(void*) {
      uint32_t lastAttempt = 0;
      for (;;) {
        if (reconfigure) {
          reconfigure = false;
          app::cfgLock();
          cfg = g_cfg.lightning;
          lat = g_cfg.location.lat; lon = g_cfg.location.lon;
          app::cfgUnlock();
          buildTopics();
          if (mqtt.connected()) mqtt.disconnect();
          mqtt.setServer(cfg.server, cfg.port);
          mqtt.setBufferSize(1024);
          mqtt.setKeepAlive(60);
          mqtt.setCallback(onMessage);
          lastAttempt = 0;
        }
        if (!cfg.enabled || !wifi_mgr::isConnected()) { if (mqtt.connected()) mqtt.disconnect(); delay(1000); continue; }
        if (!mqtt.connected()) {
          uint32_t now = millis();
          if (now - lastAttempt < 30000 && lastAttempt) { delay(500); continue; }
          lastAttempt = now;
          char id[24];
          uint8_t mac[6]; WiFi.macAddress(mac);
          snprintf(id, sizeof(id), "mwc-%02X%02X%02X", mac[3], mac[4], mac[5]);
          if (mqtt.connect(id)) {
            for (uint8_t i = 0; i < ntopics; i++) mqtt.subscribe(topics[i].c_str());
            LOGI("lightning: connected to %s, %u cells around %.2f,%.2f", cfg.server, ntopics, lat, lon);
          } else {
            LOGW("lightning: connect failed (%d)", mqtt.state());
            delay(1000);
            continue;
          }
        }
        mqtt.loop();
        delay(10);
      }
    }
  }

  void begin() {
    mtx = xSemaphoreCreateMutex();
    memset(recentTimes, 0, sizeof(recentTimes));
    xTaskCreatePinnedToCore(task, "lightning", 6144, nullptr, 1, nullptr, 0);
  }

  void applyConfig() { reconfigure = true; }

  Status status() {
    Status st = {};
    st.enabled = cfg.enabled;
    st.connected = mqtt.connected();
    st.total = total;
    time_t now = time(nullptr);
    time_t cutoff = now - (time_t)cfg.window_min * 60;
    if (take()) {
      float nearest = 1e9f;
      time_t latest = 0;
      for (size_t i = 0; i < count; i++) {
        const Strike& s = strikes[i];
        if (now < 1700000000 || s.t < cutoff) continue;
        st.count++;
        if (s.km < nearest) nearest = s.km;
        if (s.t >= latest) { latest = s.t; st.latest_km = s.km; st.latest_bearing = s.bearing; st.latest_time = s.t; }
      }
      st.nearest_km = st.count ? nearest : 0;
      give();
    }
    st.active = st.count > 0;
    if (!st.active) wasActive = false;
    return st;
  }

  bool consumeStrikeEvent() { bool e = strikeEvent; strikeEvent = false; return e; }
  bool consumeChimeEvent() { bool e = chimeEvent; chimeEvent = false; return e; }
  bool consumeNotifyEvent() { bool e = notifyEvent; notifyEvent = false; return e; }
  bool consumeWebhookEvent() { bool e = webhookEvent; webhookEvent = false; return e; }

  const char* bearingName(int16_t deg) {
    static const char* const N[8] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
    return N[((deg + 22) / 45) & 7];
  }
}
