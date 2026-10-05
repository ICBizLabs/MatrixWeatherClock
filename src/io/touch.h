#pragma once
#include <Arduino.h>
#include "buttons.h"

// GT911 touch controller on the 4-inch LCD board. Polled from loop(). Directions follow the picture as you see it
// (panel.rotation is taken into account).
//   tap                      next page (snoozes a ringing alarm)
//   hold 1.5 s               dismiss: stop an alarm, clear a message, acknowledge alerts
//   swipe left / right       top half: next / previous page; bottom half: next / previous lower screen
//   swipe up / down          brighter / dimmer
// While an alarm or timer rings, any swipe snoozes it like a tap.
namespace touch {
  bool begin(buttons::Handler h);   // false when the board has no touch panel or none answers
  void loop();
  bool available();
}
