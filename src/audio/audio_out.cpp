#include "audio_out.h"
#include <ESP_I2S.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <math.h>
#include <esp_random.h>
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include "es8311.h"
#include "app.h"
#include "pins.h"
#include "time/time_service.h"
#include "util/timeutil.h"
#include "util/log.h"

namespace audio_out {
  namespace {
    constexpr uint32_t RATE = 22050;
    constexpr size_t FRAMES = 256;
    constexpr const char* PACK_PATH = "/voice.pack";
    enum class ReqType : uint8_t { Chime, Clip };
    struct Request { ReqType type; ChimeStyle style; ClipRef clip[MAX_SEQ]; uint8_t nclips; uint8_t repeat; uint16_t preGapMs; };
    I2SClass i2s;
    AudioConfig cfg;
    QueueHandle_t q = nullptr;
    bool ready = false;
    volatile bool playing = false;
    SemaphoreHandle_t playMtx = nullptr;                // play() is called from the main loop, the net task and async_tcp
    int16_t sine[256];
    int16_t buf[FRAMES * 2];
    uint8_t* adpcmIn = nullptr;                          // 2 KB clip read buffer in PSRAM (allocated in begin)
    constexpr size_t ADPCM_IN = 2048;
    const char* suppressReason = "";

    void writeSilence(uint32_t ms) {
      memset(buf, 0, sizeof(buf));
      uint32_t frames = RATE * ms / 1000;
      while (frames) {
        size_t n = frames > FRAMES ? FRAMES : frames;
        i2s.write((uint8_t*)buf, n * 4);
        frames -= n;
      }
    }

    // Plays a tone with a 5 ms attack/release envelope. Sweeps from f0 to f1 over the duration.
    void tone(float f0, float f1, uint32_t ms, float amp) {
      uint32_t total = RATE * ms / 1000, done = 0;
      uint32_t phase = 0;                       // 32-bit phase accumulator; the top 8 bits index the sine table
      const uint32_t env = RATE * 5 / 1000;     // envelope length in frames
      while (done < total) {
        size_t n = (total - done) > FRAMES ? FRAMES : (total - done);
        for (size_t i = 0; i < n; i++) {
          uint32_t k = done + i;
          float f = f0 + (f1 - f0) * (float)k / (float)total;
          uint32_t inc = (uint32_t)(f * 4294967296.0f / RATE);       // one full table cycle = 2^32
          phase += inc;
          float e = 1.0f;
          if (k < env) e = (float)k / env;
          else if (total - k < env) e = (float)(total - k) / env;
          int16_t s = (int16_t)(sine[(phase >> 24) & 0xFF] * amp * e);
          buf[2 * i] = s;
          buf[2 * i + 1] = s;
        }
        i2s.write((uint8_t*)buf, n * 4);
        done += n;
      }
    }

    // Two simultaneous sines (the EAS attention signal is 853 Hz + 960 Hz)
    void dualTone(float fa, float fb, uint32_t ms, float amp) {
      uint32_t total = RATE * ms / 1000, done = 0, pa = 0, pb = 0;
      const uint32_t ia = (uint32_t)(fa * 4294967296.0f / RATE), ib = (uint32_t)(fb * 4294967296.0f / RATE);
      const uint32_t env = RATE * 8 / 1000;
      while (done < total) {
        size_t n = (total - done) > FRAMES ? FRAMES : (total - done);
        for (size_t i = 0; i < n; i++) {
          uint32_t k = done + i;
          pa += ia; pb += ib;
          float e = 1.0f;
          if (k < env) e = (float)k / env; else if (total - k < env) e = (float)(total - k) / env;
          int16_t v = (int16_t)((sine[(pa >> 24) & 0xFF] + sine[(pb >> 24) & 0xFF]) * 0.5f * amp * e);
          buf[2 * i] = v; buf[2 * i + 1] = v;
        }
        i2s.write((uint8_t*)buf, n * 4);
        done += n;
      }
    }

    // A struck note: exponential decay with time constant tau_ms
    void decayTone(float f, uint32_t ms, float amp, float tau_ms) {
      uint32_t total = RATE * ms / 1000, done = 0, phase = 0;
      const uint32_t inc = (uint32_t)(f * 4294967296.0f / RATE);
      const float k = -1000.0f / (tau_ms * RATE);
      while (done < total) {
        size_t n = (total - done) > FRAMES ? FRAMES : (total - done);
        for (size_t i = 0; i < n; i++) {
          uint32_t t = done + i;
          phase += inc;
          float e = expf(k * (float)t);
          if (t < 40) e *= (float)t / 40.0f;
          int16_t v = (int16_t)(sine[(phase >> 24) & 0xFF] * amp * e);
          buf[2 * i] = v; buf[2 * i + 1] = v;
        }
        i2s.write((uint8_t*)buf, n * 4);
        done += n;
      }
    }

    // SAME-style data burst: 520.83 baud FSK between 2083.3 Hz (mark) and 1562.5 Hz (space) with random bits
    void fskBurst(uint32_t ms, float amp) {
      uint32_t total = RATE * ms / 1000, done = 0, phase = 0;
      const uint32_t incMark = (uint32_t)(2083.3f * 4294967296.0f / RATE), incSpace = (uint32_t)(1562.5f * 4294967296.0f / RATE);
      const float bitFrames = RATE / 520.83f;
      float nextBit = 0; uint32_t inc = incMark;
      while (done < total) {
        size_t n = (total - done) > FRAMES ? FRAMES : (total - done);
        for (size_t i = 0; i < n; i++) {
          uint32_t t = done + i;
          if ((float)t >= nextBit) { inc = random(2) ? incMark : incSpace; nextBit += bitFrames; }
          phase += inc;
          float e = (t < 40) ? (float)t / 40.0f : (total - t < 40 ? (float)(total - t) / 40.0f : 1.0f);
          int16_t v = (int16_t)(sine[(phase >> 24) & 0xFF] * amp * e);
          buf[2 * i] = v; buf[2 * i + 1] = v;
        }
        i2s.write((uint8_t*)buf, n * 4);
        done += n;
      }
    }

    void morse(const char* code, float f, uint32_t unit_ms, float amp) {   // '.' '-' ' ' (letter gap)
      for (const char* c = code; *c; c++) {
        if (*c == '.') { tone(f, f, unit_ms, amp); writeSilence(unit_ms); }
        else if (*c == '-') { tone(f, f, unit_ms * 3, amp); writeSilence(unit_ms); }
        else writeSilence(unit_ms * 2);
      }
    }

    void play(ChimeStyle style) {
      const float A = 0.6f;
      switch (style) {
        case ChimeStyle::TwoTone:
          for (int r = 0; r < 2; r++) { tone(880, 880, 150, A); writeSilence(40); tone(1175, 1175, 150, A); writeSilence(120); }
          break;
        case ChimeStyle::TripleBeep:
          for (int r = 0; r < 3; r++) { tone(1000, 1000, 80, A); writeSilence(70); }
          break;
        case ChimeStyle::Chirp:
          tone(600, 1400, 300, A); writeSilence(60); tone(600, 1400, 300, A);
          break;
        case ChimeStyle::EasAttention:
          dualTone(853, 960, 8000, A);
          break;
        case ChimeStyle::EasFull:
          for (int r = 0; r < 3; r++) { fskBurst(1000, 0.5f); writeSilence(1000); }
          dualTone(853, 960, 8000, A);
          writeSilence(1000);
          for (int r = 0; r < 3; r++) { fskBurst(400, 0.5f); writeSilence(1000); }
          break;
        case ChimeStyle::Nws1050:
          tone(1050, 1050, 5000, A);
          break;
        case ChimeStyle::SirenWail:
          for (int r = 0; r < 2; r++) { tone(500, 1200, 2000, A); tone(1200, 500, 2000, A); }
          break;
        case ChimeStyle::SirenYelp:
          for (int r = 0; r < 8; r++) { tone(600, 1300, 200, A); tone(1300, 600, 200, A); }
          break;
        case ChimeStyle::SirenHiLo:
          for (int r = 0; r < 4; r++) { tone(500, 500, 500, A); tone(700, 700, 500, A); }
          break;
        case ChimeStyle::AlarmBeeps:
          for (int r = 0; r < 3; r++) { for (int b = 0; b < 4; b++) { tone(1000, 1000, 70, A); writeSilence(70); } writeSilence(450); }
          break;
        case ChimeStyle::Doorbell:
          decayTone(659, 600, 0.7f, 250); decayTone(523, 900, 0.7f, 350);
          break;
        case ChimeStyle::Sos:
          morse("... --- ...", 800, 100, A);
          break;
        case ChimeStyle::Arpeggio:
          for (int r = 0; r < 2; r++) { decayTone(523, 150, 0.6f, 120); decayTone(659, 150, 0.6f, 120); decayTone(784, 150, 0.6f, 120); decayTone(1047, 320, 0.6f, 200); writeSilence(120); }
          break;
        case ChimeStyle::Sonar:
          decayTone(1000, 900, 0.6f, 250); writeSilence(300); decayTone(1000, 600, 0.3f, 200); writeSilence(300); decayTone(1000, 400, 0.15f, 150);
          break;
        default: break;
      }
    }

    // ---- IMA ADPCM clip player (voice pack) ----
    const int16_t STEP[89] = { 7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
      107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166,
      1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493,
      10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767 };
    const int8_t IDX[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };
    struct Adpcm { int pred = 0; int idx = 0; };
    inline int16_t adpcmStep(Adpcm& st, uint8_t code) {
      int step = STEP[st.idx], d = step >> 3;
      if (code & 4) d += step;
      if (code & 2) d += step >> 1;
      if (code & 1) d += step >> 2;
      int p = st.pred + ((code & 8) ? -d : d);
      st.pred = p > 32767 ? 32767 : (p < -32768 ? -32768 : p);
      int i = st.idx + IDX[code & 7];
      st.idx = i < 0 ? 0 : (i > 88 ? 88 : i);
      return (int16_t)st.pred;
    }

    // G.711 mu-law: one byte per sample, decoded through a table built at start-up
    int16_t ulaw[256];
    void buildUlaw() {
      for (int i = 0; i < 256; i++) {
        int b = ~i & 0xFF;
        int exp = (b >> 4) & 7;
        int s = ((((b & 0x0F) << 3) + 0x84) << exp) - 0x84;
        ulaw[i] = (int16_t)((b & 0x80) ? -s : s);
      }
    }

    void playClip(const ClipRef& c) {
      if (!adpcmIn) return;
      File f = LittleFS.open(PACK_PATH, "r");
      if (!f || !f.seek(c.offset)) { LOGW("audio: clip open/seek failed"); return; }
      Adpcm st;
      uint32_t left = c.samples, bytesLeft = c.bytes;
      size_t fill = 0;
      while (left && bytesLeft) {
        size_t want = bytesLeft < ADPCM_IN ? (size_t)bytesLeft : ADPCM_IN;
        size_t n = f.read(adpcmIn, want);
        if (!n) break;
        bytesLeft -= n;
        if (c.codec == 1) {
          for (size_t b = 0; b < n && left; b++) {
            for (int nib = 0; nib < 2 && left; nib++, left--) {
              int16_t v = adpcmStep(st, (adpcmIn[b] >> (nib ? 4 : 0)) & 0xF);
              buf[2 * fill] = v; buf[2 * fill + 1] = v;   // mono -> both channels
              if (++fill == FRAMES) { i2s.write((uint8_t*)buf, FRAMES * 4); fill = 0; }
            }
          }
        } else {
          for (size_t b = 0; b < n && left; b++, left--) {
            int16_t v = ulaw[adpcmIn[b]];
            buf[2 * fill] = v; buf[2 * fill + 1] = v;
            if (++fill == FRAMES) { i2s.write((uint8_t*)buf, FRAMES * 4); fill = 0; }
          }
        }
      }
      if (fill) i2s.write((uint8_t*)buf, fill * 4);
      f.close();
    }

    void task(void*) {
      Request r;
      for (;;) {
        if (xQueueReceive(q, &r, portMAX_DELAY) != pdTRUE) continue;
        playing = true;
        app::trace("audio", "play");
        es8311::mute(false);
        digitalWrite(pins::PA_EN, HIGH);
        delay(20);
        writeSilence(200);
        do {                                   // drain the sequence with the amplifier on: chime, gap, clip(s)
          if (r.type == ReqType::Chime) play(r.style);
          else for (uint8_t i = 0; i < r.repeat; i++) {
            writeSilence(i ? 400 : r.preGapMs);
            for (uint8_t k = 0; k < r.nclips; k++) { if (k) writeSilence(40); playClip(r.clip[k]); }
          }
        } while (xQueueReceive(q, &r, pdMS_TO_TICKS(300)) == pdTRUE);
        writeSilence(100);
        digitalWrite(pins::PA_EN, LOW);
        es8311::mute(!cfg.enabled);
        app::trace("audio", "idle");
        playing = false;
      }
    }
  }

  bool begin(const AudioConfig& ac, uint8_t codecAddr) {
    cfg = ac;
    if (!pins::HAS_AUDIO) { LOGI("audio: none on this board"); return false; }
    pinMode(pins::PA_EN, OUTPUT);
    digitalWrite(pins::PA_EN, LOW);
    if (!codecAddr) { LOGW("audio: no ES8311 found, audio disabled"); return false; }
    for (int i = 0; i < 256; i++) sine[i] = (int16_t)(sinf(2.0f * (float)M_PI * i / 256.0f) * 32000.0f);
    buildUlaw();
    adpcmIn = (uint8_t*)heap_caps_malloc(ADPCM_IN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    i2s.setPins(pins::I2S_BCLK, pins::I2S_LRCK, pins::I2S_DOUT, pins::I2S_DIN, pins::I2S_MCLK);
    if (!i2s.begin(I2S_MODE_STD, RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO)) { LOGE("audio: I2S begin failed"); return false; }
    if (!es8311::init(codecAddr, RATE)) return false;
    es8311::setVolume(cfg.volume);
    es8311::mute(true);
    q = xQueueCreate(2, sizeof(Request));      // one sequence (chime + clip) at most; long sounds are not stacked
    playMtx = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(task, "audio", 8192, nullptr, 3, nullptr, 0);
    ready = true;
    LOGI("audio: ready (volume %u%%)", cfg.volume);
    return true;
  }

  void apply(const AudioConfig& ac) {
    cfg = ac;
    if (ready) es8311::setVolume(cfg.volume);
  }

  bool available() { return ready; }

  bool inQuietHours() {
    struct tm lt;
    if (!cfg.quiet.enabled || !timesvc::localNow(lt)) return false;
    return in_window(cfg.quiet.start, cfg.quiet.end, (uint16_t)(lt.tm_hour * 60 + lt.tm_min));
  }

  namespace {
    // one queued chime plus one queued utterance; the task drains both with the amplifier on
    bool playSeqInner(ChimeStyle style, const ClipRef* clips, uint8_t n, uint8_t repeat, bool force) {
      (void)force;
      if (n > MAX_SEQ) n = MAX_SEQ;
      Request rc = { ReqType::Chime, style, {}, 0, 0, 0 };
      Request rv = { ReqType::Clip, ChimeStyle::None, {}, n, (uint8_t)(repeat < 1 ? 1 : (repeat > 3 ? 3 : repeat)),
                     (uint16_t)(style != ChimeStyle::None ? 250 : 0) };
      for (uint8_t i = 0; i < n; i++) rv.clip[i] = clips[i];
      if (xSemaphoreTake(playMtx, pdMS_TO_TICKS(50)) != pdTRUE) { suppressReason = "busy"; return false; }
      bool ok = !playing && uxQueueMessagesWaiting(q) == 0;
      if (ok) {
        if (style != ChimeStyle::None) xQueueSend(q, &rc, 0);
        if (n) xQueueSend(q, &rv, 0);
      }
      xSemaphoreGive(playMtx);
      if (!ok) suppressReason = "busy";
      return ok;
    }
  }

  bool play(ChimeStyle style, const ClipRef* clip, uint8_t repeat, bool force) {
    suppressReason = "";
    if (!ready) { suppressReason = "no audio"; return false; }
    if (style == ChimeStyle::None && !clip) { suppressReason = "chime style none"; return false; }
    if (!force && !cfg.enabled) { suppressReason = "audio disabled"; LOGI("sound suppressed: audio disabled"); return false; }
    if (!force && inQuietHours()) { suppressReason = "quiet hours"; LOGI("sound suppressed: quiet hours"); return false; }
    return playSeqInner(style, clip, clip ? 1 : 0, repeat, force);
  }

  bool playSeq(ChimeStyle style, const ClipRef* clips, uint8_t n, bool force) {
    suppressReason = "";
    if (!ready) { suppressReason = "no audio"; return false; }
    if (style == ChimeStyle::None && (!clips || !n)) { suppressReason = "chime style none"; return false; }
    if (!force && !cfg.enabled) { suppressReason = "audio disabled"; LOGI("sound suppressed: audio disabled"); return false; }
    if (!force && inQuietHours()) { suppressReason = "quiet hours"; LOGI("sound suppressed: quiet hours"); return false; }
    return playSeqInner(style, clips, n, 1, force);
  }

  bool chime(ChimeStyle style, bool force) { return play(style, nullptr, 0, force); }

  bool busy() { return playing || (q && uxQueueMessagesWaiting(q) > 0); }

  const char* lastSuppressReason() { return suppressReason; }
}
