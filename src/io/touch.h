#pragma once
#include <Arduino.h>
#include "buttons.h"

// GT911 touch controller on the 4-inch LCD board, used as one big key. Polled from loop().
// A tap acts like a short K1 press (next page; snooze a ringing alarm). Holding for 1.5 s acts like a short K2
// press (dismiss: stop an alarm, clear a message, acknowledge alerts).
namespace touch {
  bool begin(buttons::Handler h);   // false when the board has no touch panel or none answers
  void loop();
  bool available();
}
