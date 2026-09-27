#include "alarm.h"
#include <time.h>
#include "config/config.h"
#include "audio/audio_out.h"
#include "audio/voice.h"
#include "net/pushbullet.h"
#include "net/webhook.h"
#include "time/time_service.h"
#include "util/log.h"

namespace alarmclock {
  namespace {
    constexpr uint32_t RING_REPEAT_MS = 6000;
    constexpr uint32_t RING_TIMEOUT_MS = 10 * 60000UL;
    constexpr uint32_t SNOOZE_SEC = 9 * 60;      // fallback when an alarm has no snooze_min of its own
    constexpr uint32_t STRIKE_MS = 1100;         // a struck hour sounds about one beat per second

    bool isRinging = false, ringTimer = false;
    ChimeStyle ringStyle = ChimeStyle::TripleBeep;
    char ringLabel[16] = "";
    uint32_t ringSince = 0, lastRing = 0;
    int32_t lastFiredKey[MAX_ALARMS] = { -1, -1, -1, -1 };   // day-of-year * 1440 + minute, so each alarm fires once
    time_t snoozeAt = 0;
    uint32_t snoozeSec = SNOOZE_SEC;              // from the alarm that is ringing
    uint32_t timerEndMs = 0;
    bool timerActive = false;

    uint32_t swStart = 0, swHeld = 0;              // stopwatch: start millis, and elapsed while paused
    bool swRunning = false, swUsed = false;

    int8_t lastChimeHour = -1, lastChimeHalf = -1; // so the hour chimes once even though loop() runs every second
    uint8_t strikesLeft = 0;
    uint32_t nextStrike = 0;
    ChimeStyle strikeStyle = ChimeStyle::Doorbell;
    bool sayAfterStrike = false;
    int8_t sayHour = 0, sayMin = 0;

    void startRing(bool timer, ChimeStyle style, const char* label) {
      isRinging = true;
      ringTimer = timer;
      ringStyle = style == ChimeStyle::None ? ChimeStyle::TripleBeep : style;
      strlcpy(ringLabel, label ? label : "", sizeof(ringLabel));
      ringSince = millis();
      lastRing = 0;
      LOGI("alarm: %s ringing%s%s", timer ? "timer" : "alarm", ringLabel[0] ? " " : "", ringLabel);
      // first ring: chime + spoken "Alarm" / "Timer finished"; the repeats in loop() stay chime-only
      if (voice::announce(voice::Kind::Alarm, ringStyle, timer ? "timer finished" : "alarm", true)) lastRing = millis();
      const char* what = timer ? "Timer done" : "Alarm";
      const char* detail = ringLabel[0] ? ringLabel : (timer ? "The countdown finished" : "Alarm is ringing");
      if (g_cfg.pushbullet.notify_alarms) pushbullet::notify(what, detail);
      if (webhook::wants(webhook::Event::Alarm, Severity::Unknown)) webhook::notify(webhook::Event::Alarm, what, detail);
    }
  }

  namespace {
    // Called once a second with a valid local time. Quiet hours are enforced inside audio_out, so a chime during
    // them is simply suppressed; nothing here needs to know the window.
    void hourlyChime(const struct tm& lt) {
      const AudioConfig& a = g_cfg.audio;
      if (!a.hourly_chime) { lastChimeHour = lastChimeHalf = -1; strikesLeft = 0; return; }
      if (isRinging || snoozeAt) return;                       // never compete with a ringing alarm
      const ChimeStyle style = a.hourly_style == ChimeStyle::None ? ChimeStyle::Doorbell : a.hourly_style;
      if (lt.tm_min == 0 && lastChimeHour != lt.tm_hour) {
        lastChimeHour = (int8_t)lt.tm_hour;
        strikeStyle = style;
        sayAfterStrike = a.hourly_speak;
        sayHour = (int8_t)lt.tm_hour;
        sayMin = 0;
        if (a.hourly_strike) {
          int h = lt.tm_hour % 12;
          strikesLeft = (uint8_t)(h == 0 ? 12 : h);
          nextStrike = millis();                               // first strike on this tick
        } else {
          strikesLeft = 0;
          audio_out::chime(style, false);
          if (sayAfterStrike) { sayAfterStrike = false; voice::sayTime(ChimeStyle::None, sayHour, 0, false); }
        }
        LOGI("clock: hourly chime at %02d:00", lt.tm_hour);
      } else if (a.hourly_half && lt.tm_min == 30 && lastChimeHalf != lt.tm_hour) {
        lastChimeHalf = (int8_t)lt.tm_hour;
        audio_out::chime(style, false);
      }
    }

    void strikeLoop(uint32_t now_ms) {
      if (!strikesLeft) return;
      if (isRinging) { strikesLeft = 0; sayAfterStrike = false; return; }
      if ((int32_t)(now_ms - nextStrike) < 0) return;
      if (audio_out::busy()) return;                            // one strike at a time
      audio_out::chime(strikeStyle, false);
      nextStrike = now_ms + STRIKE_MS;
      if (--strikesLeft == 0 && sayAfterStrike) {
        sayAfterStrike = false;
        voice::sayTime(ChimeStyle::None, sayHour, sayMin, false);
      }
    }
  }

  void begin() {}

  void loop(uint32_t now_ms) {
    struct tm lt;
    const bool tv = timesvc::localNow(lt);
    if (tv) {
      const int32_t key = lt.tm_yday * 1440 + lt.tm_hour * 60 + lt.tm_min;
      const uint8_t dayBit = (uint8_t)(1 << ((lt.tm_wday + 6) % 7));   // tm_wday: 0 = Sunday -> bit 6
      for (uint8_t i = 0; i < MAX_ALARMS; i++) {
        const AlarmConfig& a = g_cfg.alarms.items[i];
        if (!a.enabled || !(a.days & dayBit) || a.minute != (uint16_t)(lt.tm_hour * 60 + lt.tm_min) || lastFiredKey[i] == key) continue;
        lastFiredKey[i] = key;
        snoozeAt = 0;
        snoozeSec = (uint32_t)(a.snooze_min ? a.snooze_min : 9) * 60;
        if (!isRinging) startRing(false, a.chime, a.label);
        if (a.once) {                                  // a one-off alarm disarms itself and the change is persisted
          g_cfg.alarms.items[i].enabled = false;
          config_save(g_cfg);
          LOGI("alarm: one-off %u fired and is now off", (unsigned)(i + 1));
        }
      }
      if (snoozeAt && !isRinging && time(nullptr) >= snoozeAt) { snoozeAt = 0; startRing(false, ringStyle, ringLabel); }
      hourlyChime(lt);
    }
    if (timerActive && (int32_t)(now_ms - timerEndMs) >= 0) {
      timerActive = false;
      startRing(true, g_cfg.audio.chime == ChimeStyle::None ? ChimeStyle::TripleBeep : g_cfg.audio.chime, "TIMER");
    }
    strikeLoop(now_ms);
    if (isRinging) {
      if (now_ms - ringSince > RING_TIMEOUT_MS) { LOGI("alarm: ring timed out"); isRinging = false; return; }
      if (lastRing == 0 || now_ms - lastRing >= RING_REPEAT_MS) { lastRing = now_ms; audio_out::chime(ringStyle, true); }
    }
  }

  bool ringing() { return isRinging; }
  bool ringingIsTimer() { return ringTimer; }
  const char* ringingLabel() { return ringLabel; }

  void stop() {
    if (isRinging) LOGI("alarm: stopped");
    isRinging = false;
    snoozeAt = 0;
  }

  void snooze() {
    if (!isRinging || ringTimer) { stop(); return; }
    isRinging = false;
    snoozeAt = time(nullptr) + (time_t)snoozeSec;
    LOGI("alarm: snoozed for %u min", (unsigned)(snoozeSec / 60));
  }

  bool snoozed() { return snoozeAt != 0; }
  uint32_t snoozeRemainingSec() { time_t n = time(nullptr); return (snoozeAt && snoozeAt > n) ? (uint32_t)(snoozeAt - n) : 0; }

  bool startTimer(uint32_t seconds) {
    if (seconds == 0 || seconds > 86400) return false;
    timerEndMs = millis() + seconds * 1000UL;
    timerActive = true;
    LOGI("alarm: timer started, %lu s", (unsigned long)seconds);
    return true;
  }

  void stopwatchStart() { if (!swRunning) { swStart = millis(); swRunning = true; swUsed = true; LOGI("stopwatch: started"); } }
  void stopwatchStop() { if (swRunning) { swHeld += millis() - swStart; swRunning = false; LOGI("stopwatch: %lu ms", (unsigned long)swHeld); } }
  void stopwatchReset() { swRunning = false; swHeld = 0; swUsed = false; LOGI("stopwatch: reset"); }
  void stopwatchToggle() { if (swRunning) stopwatchStop(); else stopwatchStart(); }
  bool stopwatchRunning() { return swRunning; }
  bool stopwatchActive() { return swRunning || (swUsed && swHeld); }
  uint32_t stopwatchMs() { return swHeld + (swRunning ? millis() - swStart : 0); }
  bool striking() { return strikesLeft > 0; }

  void cancelTimer() { timerActive = false; if (isRinging && ringTimer) isRinging = false; }
  bool timerRunning() { return timerActive; }
  uint32_t timerRemainingSec() { if (!timerActive) return 0; int32_t d = (int32_t)(timerEndMs - millis()); return d > 0 ? (uint32_t)(d + 999) / 1000 : 0; }
}
