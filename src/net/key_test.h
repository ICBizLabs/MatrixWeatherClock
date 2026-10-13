#pragma once
#include <Arduino.h>
#include "config/config.h"

// Checks an API key without saving it: one harmless "who am I" request to the service (Pushbullet users/me, OpenXBL
// account), made on the network task. The web UI starts a test with the key as typed and polls for the result; an
// empty key or "***" tests the stored one.
namespace key_test {
  struct Result {
    bool running;                    // started, not finished yet
    bool done;                       // a result is available
    bool ok;
    char service[12];
    char msg[64];                    // the account name on success, otherwise why it failed
  };
  bool start(const char* service, const char* key);   // false: unknown service, or a test still running
  void run(const AppConfig& cfg);                      // network task
  Result result();
}
