#pragma once
#include <Arduino.h>

// FreeRTOS task on core 0 that performs all HTTP(S) work (weather, alerts) with per-job backoff.
namespace net_task {
  enum Job : uint8_t { JOB_WEATHER = 1, JOB_ALERTS = 2, JOB_PUSH = 4, JOB_UPDATE = 8, JOB_RADAR = 16, JOB_TIDE = 32,
                       JOB_WEBHOOK = 64 };   // 128 is the last free bit; widen kick() and the queue beyond that
  void start();
  void pause(bool paused);         // OTA / reboot
  void kick(uint8_t jobs);         // run jobs as soon as possible
}
