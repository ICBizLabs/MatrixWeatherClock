#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "config/config.h"
#include "audio/audio_out.h"

// Spoken announcements: looks phrases up in the voice pack (/voice.pack in LittleFS, built by tools/make_voice_pack.py
// and downloaded by net/voice_pack.cpp) and plays them through audio_out after the chime.
namespace voice {
  enum class Kind : uint8_t { Alert, Lightning, Alarm, Demo, Indoor, Test, Time };
  struct PackInfo {
    bool installed = false;
    uint32_t version = 0;
    uint16_t format = 0;
    uint16_t clips = 0;
    uint32_t rate = 0;
    char voice[32] = "";
  };
  void begin(const AudioConfig& ac);            // after LittleFS.begin() and audio_out::begin(): loads the pack index
  void apply(const AudioConfig& ac);            // CHG_AUDIO
  bool reload();                                // (re)read /voice.pack; false = no or invalid pack
  void lock(bool on);                           // refuse lookups while the pack file is being replaced
  PackInfo info();
  bool lookup(const char* phrase, audio_out::ClipRef& out);            // normalized phrase -> clip
  bool say(const char* phrase, bool force);                            // clip only (test); false + lastError() on failure
  // Chime (None = no chime) followed by the phrase when speech is enabled for this kind and the pack has it;
  // falls back to "weather alert" for unknown alert events and to the chime alone when there is no clip.
  bool announce(Kind kind, ChimeStyle style, const char* phrase, bool force);
  // Speaks a wall-clock time by stringing number clips together ("it is" + hour + minute + AM/PM), so the pack
  // carries about thirty small words instead of a clip for every minute of the day.
  bool sayTime(ChimeStyle style, int hour, int minute, bool force);
  bool canSayTime();                            // the pack has the number clips
  const char* lastError();
  size_t phrases(JsonArray out);                // every phrase in the pack (for /api/voice/phrases)
}
