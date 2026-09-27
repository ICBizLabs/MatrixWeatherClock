#include "time_service.h"
#include <sys/time.h>
#include <esp_sntp.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "pcf85063.h"
#include "util/log.h"

namespace timesvc {
  namespace {
    Source src = Source::None;
    time_t lastSync = 0;
    bool rtcPresent = false;
    bool sntpStarted = false;
    SemaphoreHandle_t tzMtx = nullptr;

    void setTz(const char* posix) {
      if (tzMtx) xSemaphoreTakeRecursive(tzMtx, pdMS_TO_TICKS(250));
      setenv("TZ", posix && *posix ? posix : "UTC0", 1);
      tzset();
      if (tzMtx) xSemaphoreGiveRecursive(tzMtx);
    }
  }

  void begin(const TimeConfig& tc) {
    tzMtx = xSemaphoreCreateRecursiveMutex();
    setTz(tc.tz_posix);
    rtcPresent = pcf85063::present();
    time_t utc;
    if (rtcPresent && pcf85063::read(utc)) {
      struct timeval tv = { utc, 0 };
      settimeofday(&tv, nullptr);
      src = Source::Rtc;
      LOGI("time: set from RTC (%ld)", (long)utc);
    } else {
      LOGI("time: RTC %s", rtcPresent ? "present but not set" : "not found");
    }
  }

  void onWifiUp(const TimeConfig& tc) {
    configTzTime(tc.tz_posix, tc.ntp1, tc.ntp2[0] ? tc.ntp2 : nullptr);
    sntpStarted = true;
    LOGI("time: SNTP started (%s)", tc.ntp1);
  }

  void applyTz(const TimeConfig& tc) {
    setTz(tc.tz_posix);
    if (sntpStarted) configTzTime(tc.tz_posix, tc.ntp1, tc.ntp2[0] ? tc.ntp2 : nullptr);
  }

  void loop() {
    if (sntpStarted && sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
      lastSync = time(nullptr);
      src = Source::Ntp;
      LOGI("time: NTP sync %ld", (long)lastSync);
      if (rtcPresent) {
        if (pcf85063::write(lastSync)) LOGI("time: RTC updated");
        else LOGW("time: RTC write failed");
      }
    }
  }

  void tzLock() { if (tzMtx) xSemaphoreTakeRecursive(tzMtx, pdMS_TO_TICKS(250)); }
  void tzUnlock() { if (tzMtx) xSemaphoreGiveRecursive(tzMtx); }

  bool zoneNow(const char* posix, struct tm& out) {
    if (!posix || !*posix) return false;
    time_t now = time(nullptr);
    if (now < 1700000000) return false;
    char saved[80];
    tzLock();
    const char* cur = getenv("TZ");
    strlcpy(saved, cur ? cur : "UTC0", sizeof(saved));
    setenv("TZ", posix, 1);
    tzset();
    localtime_r(&now, &out);
    setenv("TZ", saved, 1);
    tzset();
    tzUnlock();
    return true;
  }

  bool valid() { return time(nullptr) > 1700000000; }

  bool localNow(struct tm& lt, uint16_t* ms) {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    if (tv.tv_sec < 1700000000) return false;
    tzLock();
    localtime_r(&tv.tv_sec, &lt);
    tzUnlock();
    if (ms) *ms = (uint16_t)(tv.tv_usec / 1000);
    return true;
  }

  Status status() { return { src, lastSync, rtcPresent }; }

  const char* sourceName(Source s) {
    switch (s) { case Source::Rtc: return "rtc"; case Source::Ntp: return "ntp"; default: return "none"; }
  }
}
