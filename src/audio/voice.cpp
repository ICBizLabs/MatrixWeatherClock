#include "voice.h"
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "util/log.h"

namespace voice {
  namespace {
    constexpr const char* PACK_PATH = "/voice.pack";
    constexpr uint32_t MAGIC = 0x5643574D;      // "MWCV" little-endian
    constexpr uint16_t FORMAT_ADPCM = 1, FORMAT_ULAW = 2;
    constexpr size_t HEADER_LEN = 72;
    constexpr uint32_t MAX_CLIPS = 1024;
    struct Entry { uint32_t key, offset, bytes, samples; };

    AudioConfig cfg;
    SemaphoreHandle_t mtx = nullptr;
    Entry* entries = nullptr;
    char* names = nullptr;
    uint32_t namesLen = 0;
    PackInfo pack;
    bool locked = false;
    const char* err = "";

    bool take() { return mtx && xSemaphoreTake(mtx, pdMS_TO_TICKS(200)) == pdTRUE; }
    void give() { xSemaphoreGive(mtx); }

    uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
    uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

    // same rules as tools/make_voice_pack.py normalize(): trim, collapse whitespace, ASCII lowercase
    size_t normalize(const char* in, char* out, size_t outLen) {
      size_t n = 0;
      bool space = false;
      for (const char* p = in; *p && n + 1 < outLen; p++) {
        char c = *p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { space = n > 0; continue; }
        if (space) { out[n++] = ' '; space = false; if (n + 1 >= outLen) break; }
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        out[n++] = c;
      }
      out[n] = '\0';
      return n;
    }
    uint32_t fnv1a(const char* s) {
      uint32_t h = 2166136261u;
      for (const uint8_t* p = (const uint8_t*)s; *p; p++) { h ^= *p; h *= 16777619u; }
      return h;
    }

    void unload() {
      if (entries) { free(entries); entries = nullptr; }
      if (names) { free(names); names = nullptr; }
      namesLen = 0;
      pack = PackInfo();
    }

    bool load() {
      unload();
      File f = LittleFS.open(PACK_PATH, "r");
      if (!f) { err = "no voice pack"; return false; }
      uint8_t h[HEADER_LEN];
      if (f.read(h, HEADER_LEN) != HEADER_LEN || rd32(h) != MAGIC) { err = "pack header invalid"; f.close(); return false; }
      const uint16_t format = rd16(h + 4), hlen = rd16(h + 6);
      const uint32_t rate = rd32(h + 8), count = rd32(h + 12), version = rd32(h + 16), indexOff = rd32(h + 20);
      const uint32_t namesOff = rd32(h + 24), nlen = rd32(h + 28), fileSize = rd32(h + 36);
      if (format != FORMAT_ADPCM && format != FORMAT_ULAW) { err = "pack format unsupported"; f.close(); return false; }
      if (hlen != HEADER_LEN || rate != 22050 || count == 0 || count > MAX_CLIPS || fileSize != (uint32_t)f.size() ||
          indexOff + 16 * count > fileSize || namesOff + nlen > fileSize) { err = "pack header invalid"; f.close(); return false; }
      entries = (Entry*)heap_caps_malloc(count * sizeof(Entry), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      names = (char*)heap_caps_malloc(nlen + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (!entries || !names) { err = "no memory for pack entries"; unload(); f.close(); return false; }
      bool ok = f.seek(indexOff) && f.read((uint8_t*)entries, count * sizeof(Entry)) == count * sizeof(Entry) &&
                f.seek(namesOff) && f.read((uint8_t*)names, nlen) == nlen;
      f.close();
      if (!ok) { err = "pack read failed"; unload(); return false; }
      names[nlen] = '\0';
      namesLen = nlen;
      pack.installed = true;
      pack.version = version;
      pack.format = format;
      pack.clips = (uint16_t)count;
      pack.rate = rate;
      memcpy(pack.voice, h + 40, 31);
      pack.voice[31] = '\0';
      err = "";
      return true;
    }

    const Entry* find(uint32_t key) {
      size_t lo = 0, hi = pack.clips;
      while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (entries[mid].key == key) return &entries[mid];
        if (entries[mid].key < key) lo = mid + 1; else hi = mid;
      }
      return nullptr;
    }

    bool lookupLocked(const char* phrase, audio_out::ClipRef& out) {
      char key[96];
      normalize(phrase, key, sizeof(key));
      const Entry* e = find(fnv1a(key));
      if (!e) return false;
      out.offset = e->offset; out.bytes = e->bytes; out.samples = e->samples; out.codec = (uint8_t)pack.format;
      return true;
    }
  }

  void begin(const AudioConfig& ac) {
    cfg = ac;
    mtx = xSemaphoreCreateMutex();
    if (load()) LOGI("voice: pack v%lu loaded (%u clips, %s)", (unsigned long)pack.version, pack.clips, pack.voice);
    else LOGI("voice: %s", err);
  }

  void apply(const AudioConfig& ac) { cfg = ac; }

  bool reload() {
    if (!take()) return false;
    bool ok = load();
    give();
    return ok;
  }

  void lock(bool on) { if (take()) { locked = on; give(); } }

  PackInfo info() { PackInfo copy; if (take()) { copy = pack; give(); } return copy; }

  bool lookup(const char* phrase, audio_out::ClipRef& out) {
    if (!take()) return false;
    bool ok = !locked && pack.installed && lookupLocked(phrase, out);
    give();
    return ok;
  }

  bool say(const char* phrase, bool force) {
    audio_out::ClipRef c;
    if (!info().installed) { err = "no voice pack"; return false; }
    if (!lookup(phrase, c)) { err = "phrase not in voice pack"; return false; }
    if (!audio_out::play(ChimeStyle::None, &c, cfg.speech.repeat, force)) { err = audio_out::lastSuppressReason(); return false; }
    return true;
  }

  bool announce(Kind kind, ChimeStyle style, const char* phrase, bool force) {
    bool want = cfg.speech.enabled && phrase && *phrase;
    switch (kind) {
      case Kind::Alert: want = want && cfg.speech.alerts; break;
      case Kind::Lightning: want = want && cfg.speech.lightning; break;
      case Kind::Alarm: want = want && cfg.speech.alarms; break;
      case Kind::Demo: want = want && cfg.speech.demo; break;
      case Kind::Indoor: want = want && cfg.speech.indoor; break;
      case Kind::Time: want = want && cfg.speech.say_time; break;
      default: break;
    }
    audio_out::ClipRef c;
    bool have = want && lookup(phrase, c);
    if (want && !have && kind == Kind::Alert) {
      LOGI("voice: no clip for \"%s\", saying \"weather alert\"", phrase);
      have = lookup("weather alert", c);
    }
    if (want && !have) LOGI("voice: %s (\"%s\")", info().installed ? "phrase not in voice pack" : "no voice pack", phrase);
    bool ok = audio_out::play(style, have ? &c : nullptr, cfg.speech.repeat, force);
    if (!ok) err = audio_out::lastSuppressReason();
    return ok;
  }

  namespace {
    const char* const NUM_WORDS[20] = { "oh", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine",
                                        "ten", "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen",
                                        "seventeen", "eighteen", "nineteen" };
    const char* const TENS_WORDS[6] = { "", "", "twenty", "thirty", "forty", "fifty" };
  }

  bool canSayTime() {
    audio_out::ClipRef c;
    return lookup("it is", c) && lookup("one", c) && lookup("o clock", c);
  }

  bool sayTime(ChimeStyle style, int hour, int minute, bool force) {
    if (!cfg.speech.enabled || !cfg.speech.say_time) { err = "spoken time off"; return false; }
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59) { err = "bad time"; return false; }
    const bool h24 = g_cfg.time.use_24h;
    const char* words[audio_out::MAX_SEQ];
    uint8_t n = 0;
    const auto push = [&](const char* w) { if (n < audio_out::MAX_SEQ) words[n++] = w; };
    push("it is");
    if (h24) {                                 // "zero zero hundred", "oh nine thirty", "twenty three forty seven"
      if (hour == 0) { push("zero"); push("zero"); }
      else if (hour < 10) { push("oh"); push(NUM_WORDS[hour]); }
      else if (hour < 20) push(NUM_WORDS[hour]);
      else { push(TENS_WORDS[2]); if (hour % 10) push(NUM_WORDS[hour % 10]); }
    } else {
      const int h12 = (hour % 12) == 0 ? 12 : hour % 12;
      push(NUM_WORDS[h12]);
    }
    if (minute == 0) push(h24 ? "hundred" : "o clock");
    else if (minute < 10) { push("oh"); push(NUM_WORDS[minute]); }
    else if (minute < 20) push(NUM_WORDS[minute]);
    else { push(TENS_WORDS[minute / 10]); if (minute % 10) push(NUM_WORDS[minute % 10]); }
    if (!h24) push(hour < 12 ? "a m" : "p m");

    audio_out::ClipRef clips[audio_out::MAX_SEQ];
    uint8_t got = 0;
    for (uint8_t i = 0; i < n; i++) {
      if (lookup(words[i], clips[got])) got++;
      else { LOGI("voice: no clip for \"%s\", cannot speak the time", words[i]); err = "voice pack has no number clips"; return false; }
    }
    bool ok = audio_out::playSeq(style, clips, got, force);
    if (!ok) err = audio_out::lastSuppressReason();
    return ok;
  }

  const char* lastError() { return err; }

  size_t phrases(JsonArray out) {
    size_t n = 0;
    if (!take()) return 0;
    if (names) {
      const char* p = names;
      const char* end = names + namesLen;
      while (p < end && n < pack.clips) {
        out.add(p);
        n++;
        p += strlen(p) + 1;
      }
    }
    give();
    return n;
  }
}
