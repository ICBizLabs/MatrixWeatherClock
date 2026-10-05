#include "voice_pack.h"
#include <LittleFS.h>
#include <MD5Builder.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_heap_caps.h>
#include "http_util.h"
#include "updater.h"
#include "wifi_manager.h"
#include "app.h"
#include "alarm/alarm.h"
#include "audio/audio_out.h"
#include "audio/voice.h"
#include "util/log.h"
#include "version.h"

namespace voice_pack {
  namespace {
    constexpr const char* PACK_PATH = "/voice.pack";
    constexpr const char* TMP_PATH = "/voice.tmp";
    constexpr size_t CHUNK = 4096;
    constexpr uint32_t RETRY_MS = 30 * 60000UL;
    SemaphoreHandle_t mtx = nullptr;
    Status st = {};
    volatile bool forced = false;
    bool askedCheck = false;
    uint32_t failedVersion = 0, failedAt = 0;

    bool take() { return mtx && xSemaphoreTake(mtx, pdMS_TO_TICKS(100)) == pdTRUE; }
    void give() { xSemaphoreGive(mtx); }
    void setState(State s, const char* err = nullptr) {
      if (!take()) return;
      st.state = s;
      if (err) strlcpy(st.err, err, sizeof(st.err)); else if (s != State::Error) st.err[0] = '\0';
      give();
    }
    void setProgress(uint8_t p) { if (take()) { st.progress = p; give(); } }

    bool download(const updater::VoiceInfo& vi) {
      if (LittleFS.totalBytes() - LittleFS.usedBytes() < vi.size + 65536) { setState(State::Error, "not enough space"); return false; }
      String url = updater::manifestBase() + vi.file;
      LOGI("voice: downloading %s (%lu bytes)", url.c_str(), (unsigned long)vi.size);
      setState(State::Downloading);
      setProgress(0);
      LittleFS.remove(TMP_PATH);
      File f = LittleFS.open(TMP_PATH, "w");
      if (!f) { setState(State::Error, "cannot create /voice.tmp"); return false; }
      http_util::Options opt;
      opt.userAgent = MWC_USER_AGENT_NAME "/" MWC_VERSION;
      opt.accept = "application/octet-stream";
      opt.timeoutMs = 30000;
      MD5Builder md5;
      md5.begin();
      String err;
      int code = 0;
      bool ok = http_util::get(url, opt, [&](Stream& s, int len) -> bool {
        if (len <= 0) { err = "unknown download size"; return false; }
        if (vi.size && (uint32_t)len != vi.size) { err = "size differs from manifest"; return false; }
        uint8_t* buf = (uint8_t*)heap_caps_malloc(CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!buf) { err = "no memory"; return false; }
        size_t got = 0;
        uint32_t lastData = millis();
        while (got < (size_t)len) {
          size_t want = min(CHUNK, (size_t)len - got);
          int n = s.readBytes(buf, want);
          if (n <= 0) {
            if (millis() - lastData > 30000) { err = "download stalled"; break; }
            delay(1);
            continue;
          }
          lastData = millis();
          md5.add(buf, (uint16_t)n);
          if (f.write(buf, n) != (size_t)n) { err = "flash write failed"; break; }
          got += n;
          setProgress((uint8_t)((uint64_t)got * 100 / len));
        }
        free(buf);
        return got == (size_t)len;
      }, err, &code);
      f.close();
      if (ok) {
        setState(State::Verifying);
        md5.calculate();
        if (vi.md5[0] && !md5.toString().equalsIgnoreCase(vi.md5)) { ok = false; err = "md5 mismatch"; }
      }
      if (!ok) {
        LittleFS.remove(TMP_PATH);
        setState(State::Error, err.c_str());
        LOGE("voice: download failed: %s", err.c_str());
        return false;
      }
      // swap the file in while no clip is being read from the old one
      voice::lock(true);
      for (uint32_t t0 = millis(); audio_out::busy() && millis() - t0 < 20000;) delay(50);
      LittleFS.remove(PACK_PATH);
      bool renamed = LittleFS.rename(TMP_PATH, PACK_PATH);
      bool loaded = renamed && voice::reload();
      voice::lock(false);
      if (!loaded) {
        if (!renamed) LittleFS.remove(TMP_PATH); else LittleFS.remove(PACK_PATH);
        setState(State::Error, renamed ? voice::lastError() : "rename failed");
        LOGE("voice: install failed: %s", renamed ? voice::lastError() : "rename failed");
        return false;
      }
      voice::PackInfo pi = voice::info();
      setState(State::Installed);
      LOGI("voice: pack v%lu installed (%u clips, %s)", (unsigned long)pi.version, pi.clips, pi.voice);
      return true;
    }
  }

  void begin() {
    mtx = xSemaphoreCreateMutex();
    st.state = voice::info().installed ? State::Installed : State::Idle;
  }

  void requestDownload() { forced = true; updater::requestCheck(); }

  void run(const AppConfig& cfg) {
    if (!wifi_mgr::isConnected() || app::rebootPending()) return;
    if (!audio_out::available()) { forced = false; return; }               // no speaker on this board: nothing to say it with
    const bool want = cfg.audio.speech.enabled || forced;
    if (!want) return;
    voice::PackInfo pi = voice::info();
    if (!updater::manifestSeen()) {
      // the updater only fetches the manifest on its own schedule; ask once when we still need a pack
      if (!pi.installed && !askedCheck) { askedCheck = true; updater::requestCheck(); }
      return;
    }
    updater::VoiceInfo vi = updater::voiceInfo();
    if (take()) { st.available_version = vi.present ? vi.version : 0; give(); }
    if (!vi.present) { if (!pi.installed) setState(State::NoPack, ""); forced = false; return; }
    if (vi.format != 1 && vi.format != 2) { setState(State::Error, "voice pack format unsupported"); forced = false; return; }
    const bool wantDownload = forced || !pi.installed || pi.version != vi.version;
    if (!wantDownload) { if (status().state != State::Installed) setState(State::Installed); return; }
    if (alarmclock::ringing()) return;                                       // try again on the next pass
    if (!forced && failedVersion == vi.version && failedAt && millis() - failedAt < RETRY_MS) return;
    forced = false;
    if (take()) { st.last_attempt_ms = millis(); give(); }
    if (!download(vi)) { failedVersion = vi.version; failedAt = millis(); }
    else { failedVersion = 0; failedAt = 0; }
  }

  Status status() { Status copy = {}; if (take()) { copy = st; give(); } return copy; }

  const char* stateName(State s) {
    switch (s) {
      case State::NoPack: return "no-pack";
      case State::Installed: return "installed";
      case State::Downloading: return "downloading";
      case State::Verifying: return "verifying";
      case State::Error: return "error";
      default: return "idle";
    }
  }
}
