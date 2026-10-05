#include "ir_remote.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <driver/gpio.h>
#include "actions.h"
#include "pins.h"
#include "util/log.h"

namespace ir_remote {
  namespace {
    constexpr size_t MAX_SYMBOLS = 96;              // two RMT memory blocks on the S3
    constexpr uint32_t TICK_HZ = 1000000;           // 1 us per tick
    constexpr uint16_t IDLE_US = 12000;             // gap that ends a frame
    constexpr uint32_t LEARN_TIMEOUT_MS = 60000;
    constexpr uint32_t REPEAT_HOLD_MS = 400;        // NEC repeat frames count while a key is held

    enum class Proto : uint8_t { None, Nec, NecRepeat, Hash };
    struct Frame { uint32_t code; Proto proto; };

    RemoteConfig cfg;
    QueueHandle_t q = nullptr;
    TaskHandle_t task = nullptr;
    int8_t activePin = -1;
    bool running = false;
    Status st = {};
    uint32_t learnSince = 0;
    uint32_t lastCode = 0, lastActionMs = 0;
    actions::Id lastAction = actions::Id::None;

    inline bool near(uint32_t v, uint32_t target, uint32_t tol) { return v + tol >= target && v <= target + tol; }

    bool decodeNec(const rmt_data_t* d, size_t n, Frame& f) {
      if (n < 2) return false;
      if (!near(d[0].duration0, 9000, 2200)) return false;
      if (near(d[0].duration1, 2250, 700) && n <= 4) { f.code = 0; f.proto = Proto::NecRepeat; return true; }
      if (!near(d[0].duration1, 4500, 1200) || n < 33) return false;
      uint32_t code = 0;
      for (int i = 1; i <= 32; i++) {
        if (!near(d[i].duration0, 560, 320)) return false;
        uint32_t sp = d[i].duration1;
        int bit;
        if (near(sp, 560, 320)) bit = 0; else if (near(sp, 1690, 500)) bit = 1; else return false;
        code = (code << 1) | (uint32_t)bit;
      }
      f.code = code; f.proto = Proto::Nec;
      return true;
    }

    // IRremote-style timing hash: compares each duration with the one two steps later, FNV-1 over the result
    bool decodeHash(const rmt_data_t* d, size_t n, Frame& f) {
      if (n < 6) return false;
      uint32_t h = 2166136261u;
      auto dur = [&](size_t i) -> uint32_t { return (i & 1) ? d[i / 2].duration1 : d[i / 2].duration0; };
      size_t total = n * 2;
      for (size_t i = 0; i + 2 < total; i++) {
        uint32_t a = dur(i), b = dur(i + 2), v;
        if (a + a / 5 < b) v = 2; else if (b + b / 5 < a) v = 0; else v = 1;
        h = (h * 16777619u) ^ v;
      }
      f.code = h ? h : 1; f.proto = Proto::Hash;
      return true;
    }

    void rxTask(void*) {
      static rmt_data_t buf[MAX_SYMBOLS];
      for (;;) {
        if (!running) { vTaskDelay(pdMS_TO_TICKS(200)); continue; }
        size_t n = MAX_SYMBOLS;
        if (!rmtRead(activePin, buf, &n, 1000)) continue;
        if (n < 2) continue;
        Frame f;
        if (!decodeNec(buf, n, f) && !decodeHash(buf, n, f)) continue;
        xQueueSend(q, &f, 0);
      }
    }

    bool startRx(int8_t pin) {
      if (pin < 0) return false;
      if (pins::reserved(pin)) { LOGW("remote: GPIO %d is in use by this board, receiver not started", pin); return false; }
      if (!rmtInit(pin, RMT_RX_MODE, RMT_MEM_NUM_BLOCKS_2, TICK_HZ)) { LOGW("remote: RMT init failed on GPIO %d", pin); return false; }
      gpio_pullup_en((gpio_num_t)pin);       // receivers idle high; keeps an unconnected pin quiet
      rmtSetRxMinThreshold(pin, 120);        // ignore glitches shorter than 120 us
      rmtSetRxMaxThreshold(pin, IDLE_US);    // a 12 ms gap ends a frame
      activePin = pin;
      running = true;
      LOGI("remote: IR receiver on GPIO %d", pin);
      return true;
    }

    void stopRx() {
      running = false;
      if (activePin >= 0) { rmtDeinit(activePin); activePin = -1; }
    }

    const char* protoName(Proto p) { return p == Proto::Nec ? "nec" : p == Proto::NecRepeat ? "nec-rpt" : p == Proto::Hash ? "hash" : "-"; }
  }

  void begin(const RemoteConfig& rc) {
    cfg = rc;
    q = xQueueCreate(8, sizeof(Frame));
    st.pin = cfg.pin;
    st.enabled = cfg.enabled;
    xTaskCreatePinnedToCore(rxTask, "ir", 3072, nullptr, 1, &task, 1);
    if (cfg.enabled) startRx(cfg.pin);
  }

  void apply(const RemoteConfig& rc) {
    const bool restart = rc.enabled != cfg.enabled || rc.pin != cfg.pin;
    cfg = rc;
    st.enabled = cfg.enabled; st.pin = cfg.pin;
    if (!restart) return;
    stopRx();
    if (cfg.enabled) startRx(cfg.pin);
  }

  void loop() {
    if (!q) return;
    Frame f;
    while (xQueueReceive(q, &f, 0) == pdTRUE) {
      const uint32_t now = millis();
      st.received++;
      if (f.proto == Proto::NecRepeat) {
        // a held key: only brightness keeps stepping
        if (lastCode && now - lastActionMs < REPEAT_HOLD_MS && (lastAction == actions::Id::BrightUp || lastAction == actions::Id::BrightDown)) {
          actions::run(lastAction, "remote-hold");
          lastActionMs = now;
        }
        strlcpy(st.last_proto, "nec-rpt", sizeof(st.last_proto));
        continue;
      }
      st.last_code = f.code;
      st.last_ms = now;
      strlcpy(st.last_proto, protoName(f.proto), sizeof(st.last_proto));
      if (learnSince && now - learnSince < LEARN_TIMEOUT_MS) {
        learnSince = 0; st.learning = false;
        LOGI("remote: learned code 0x%08lX (%s)", (unsigned long)f.code, st.last_proto);
        continue;
      }
      if (learnSince) { learnSince = 0; st.learning = false; }
      actions::Id act = actions::Id::None;
      for (uint8_t i = 0; i < cfg.nbuttons; i++) if (cfg.buttons[i].code == f.code) { act = (actions::Id)cfg.buttons[i].action; break; }
      if (act == actions::Id::None) { LOGI("remote: code 0x%08lX (%s) not mapped", (unsigned long)f.code, st.last_proto); continue; }
      lastCode = f.code; lastAction = act; lastActionMs = now;
      actions::run(act, "remote");
    }
    if (learnSince && millis() - learnSince >= LEARN_TIMEOUT_MS) { learnSince = 0; st.learning = false; }
  }

  void startLearn() { learnSince = millis() ? millis() : 1; st.learning = true; }

  Status status() { Status c = st; c.learning = learnSince != 0; return c; }
}
