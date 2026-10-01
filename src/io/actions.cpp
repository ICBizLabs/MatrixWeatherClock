#include "actions.h"
#include "app.h"
#include "config/config.h"
#include "display/renderer.h"
#include "alarm/alarm.h"
#include "audio/audio_out.h"
#include "audio/voice.h"
#include "net/alert_store.h"
#include "net/net_task.h"
#include "time/time_service.h"
#include "util/log.h"

namespace actions {
  namespace {
    struct Def { const char* name; const char* label; };
    const Def DEFS[(uint8_t)Id::COUNT] = {
      { "none", "Nothing" }, { "next_page", "Next page" }, { "dismiss", "Dismiss / OK" }, { "show_radar", "Show radar" },
      { "show_forecast", "Show forecast" }, { "show_hourly", "Show hourly graph" }, { "ack_alerts", "Acknowledge alerts" },
      { "alarm_stop", "Stop alarm / timer" }, { "alarm_snooze", "Snooze alarm" }, { "timer_5", "Timer 5 min" },
      { "timer_10", "Timer 10 min" }, { "timer_30", "Timer 30 min" }, { "timer_cancel", "Cancel timer" },
      { "bright_up", "Brighter" }, { "bright_down", "Dimmer" }, { "night", "Night mode on / off / auto" },
      { "mute", "Mute / unmute" }, { "demo", "Demo mode on / off" }, { "refresh", "Refresh weather and radar" },
      { "show_ip", "Show IP address" }, { "chime", "Play the chime" },
      { "say_time", "Speak the time" }, { "stopwatch", "Stopwatch start / pause" }, { "stopwatch_reset", "Stopwatch reset" },
      { "sleep_30", "Sleep in 30 min" }, { "sleep_60", "Sleep in 60 min" }, { "sleep_cancel", "Cancel sleep" },
      { "show_world", "Show world clock" } };
  }

  const char* name(Id id) { return (uint8_t)id < (uint8_t)Id::COUNT ? DEFS[(uint8_t)id].name : "none"; }
  const char* label(Id id) { return (uint8_t)id < (uint8_t)Id::COUNT ? DEFS[(uint8_t)id].label : "Nothing"; }

  bool parse(const char* s, Id& out) {
    if (!s) return false;
    for (uint8_t i = 0; i < (uint8_t)Id::COUNT; i++) if (!strcmp(s, DEFS[i].name)) { out = (Id)i; return true; }
    return false;
  }

  bool run(Id id, const char* source) {
    if (id == Id::None || (uint8_t)id >= (uint8_t)Id::COUNT) return false;
    LOGI("action: %s (%s)", name(id), source ? source : "?");
    switch (id) {
      case Id::NextPage: renderer::nextPage(); break;
      case Id::Dismiss:
        if (alarmclock::ringing()) alarmclock::stop();
        else if (renderer::demoActive()) renderer::setDemo(false);
        else if (renderer::hasMessage()) renderer::clearMessage();
        else alerts::acknowledge("all");
        break;
      case Id::ShowRadar: renderer::requestFullScreen("radar"); break;
      case Id::ShowForecast: renderer::requestFullScreen("forecast"); break;
      case Id::ShowHourly: renderer::requestFullScreen("hourly"); break;
      case Id::ShowWorld: renderer::requestFullScreen("world"); break;
      case Id::AckAlerts: alerts::acknowledge("all"); break;
      case Id::AlarmStop: alarmclock::stop(); break;
      case Id::AlarmSnooze: if (alarmclock::ringing()) alarmclock::snooze(); break;
      case Id::Timer5: alarmclock::startTimer(5 * 60); break;
      case Id::Timer10: alarmclock::startTimer(10 * 60); break;
      case Id::Timer30: alarmclock::startTimer(30 * 60); break;
      case Id::TimerCancel: alarmclock::cancelTimer(); break;
      case Id::BrightUp: renderer::adjustBrightness(1); break;
      case Id::BrightDown: renderer::adjustBrightness(-1); break;
      case Id::Night: renderer::cycleNightOverride(); break;
      case Id::Mute:
        app::cfgLock();
        g_cfg.audio.enabled = !g_cfg.audio.enabled;
        audio_out::apply(g_cfg.audio);
        voice::apply(g_cfg.audio);
        app::cfgUnlock();
        break;
      case Id::Demo: renderer::setDemo(!renderer::demoActive(), 10 * 60000UL, false); break;
      case Id::Refresh: net_task::kick(net_task::JOB_WEATHER | net_task::JOB_ALERTS | net_task::JOB_RADAR); break;
      case Id::ShowIp: renderer::showIp(20000); break;
      case Id::Chime: audio_out::chime(g_cfg.audio.chime == ChimeStyle::None ? ChimeStyle::TwoTone : g_cfg.audio.chime, true); break;
      case Id::SayTime: {
        struct tm lt;
        if (!timesvc::localNow(lt)) return false;
        voice::sayTime(ChimeStyle::None, lt.tm_hour, lt.tm_min, true);
        break;
      }
      case Id::StopwatchToggle: alarmclock::stopwatchToggle(); break;
      case Id::StopwatchReset: alarmclock::stopwatchReset(); break;
      case Id::Sleep30: renderer::startSleep(30); break;
      case Id::Sleep60: renderer::startSleep(60); break;
      case Id::SleepCancel: renderer::cancelSleep(); break;
      default: return false;
    }
    return true;
  }
}
