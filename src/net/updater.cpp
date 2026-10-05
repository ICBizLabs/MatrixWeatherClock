#include "updater.h"
#include <Update.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_heap_caps.h>
#include "http_util.h"
#include "net_task.h"
#include "wifi_manager.h"
#include "app.h"
#include "alarm/alarm.h"
#include "display/renderer.h"
#include "util/psram_alloc.h"
#include "util/log.h"
#include "version.h"

namespace updater {
  namespace {
    constexpr uint32_t FIRST_CHECK_MS = 90 * 1000UL;      // after boot / WiFi
    constexpr uint32_t RETRY_MS = 30 * 60000UL;           // after a failed check or install
    constexpr size_t CHUNK = 4096;
    SemaphoreHandle_t mtx = nullptr;
    Status st = {};
    VoiceInfo vinfo;
    bool seenManifest = false;
    char md5[40] = "";
    String baseUrl;                                       // manifest URL without the file name
    uint32_t nextCheck = 0;
    volatile bool checkRequested = false, installRequested = false;
    bool triedInstallFor = false;                         // auto-install attempted for st.latest

    bool take() { return mtx && xSemaphoreTake(mtx, pdMS_TO_TICKS(100)) == pdTRUE; }
    void give() { xSemaphoreGive(mtx); }
    void setState(State s, const char* err = nullptr) {
      if (!take()) return;
      st.state = s;
      if (err) strlcpy(st.err, err, sizeof(st.err)); else if (s != State::Error) st.err[0] = '\0';
      give();
    }
    void setProgress(uint8_t p) { if (take()) { st.progress = p; give(); } }

    int parsePart(const char*& p) {
      int v = 0;
      while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
      while (*p && *p != '.') p++;      // skip suffixes like "-dev"
      if (*p == '.') p++;
      return v;
    }

    bool check(const AppConfig& cfg) {
      setState(State::Checking);
      JsonDocument doc(psramAllocator());
      http_util::Options opt;
      opt.userAgent = MWC_USER_AGENT_NAME "/" MWC_VERSION;
      opt.accept = "application/json";
      String err;
      bool ok = http_util::get(cfg.update.url, opt, [&](Stream& s, int) {
        return deserializeJson(doc, s) == DeserializationError::Ok;
      }, err);
      if (take()) { st.last_check_ms = millis(); give(); }
      if (!ok) { setState(State::Error, err.c_str()); LOGW("update: check failed: %s", err.c_str()); return false; }
#if defined(MWC_BOARD_LCD4848)
      // The top-level entries are the HUB75 build. This board only takes an image listed under its own key, so a
      // HUB75 release can never be installed here (it would leave the screen dark).
      JsonObjectConst src = doc["boards"]["lcd4848"];
      if (src.isNull()) { setState(State::Error, "manifest has no build for the LCD board"); LOGI("update: manifest has no lcd4848 build"); return false; }
#else
      JsonObjectConst src = doc.as<JsonObjectConst>();
#endif
      const char* ver = src["version"] | "";
      const char* ota = src["ota"] | "";
      const char* sum = src["ota_md5"] | "";
      uint32_t size = src["ota_size"] | 0;
      if (!*ver || !*ota) { setState(State::Error, "manifest has no version/ota"); return false; }
      String url = cfg.update.url;
      int slash = url.lastIndexOf('/');
      baseUrl = slash > 0 ? url.substring(0, slash + 1) : url;
      if (take()) {
        strlcpy(st.latest, ver, sizeof(st.latest));
        strlcpy(st.file, ota, sizeof(st.file));
        st.size = size;
        give();
      }
      strlcpy(md5, sum, sizeof(md5));
      {
        VoiceInfo v;
        JsonObjectConst vo = doc["voice"];
        if (!vo.isNull()) {
          v.present = true;
          strlcpy(v.file, vo["file"] | "", sizeof(v.file));
          strlcpy(v.md5, vo["md5"] | "", sizeof(v.md5));
          strlcpy(v.voice, vo["voice"] | "", sizeof(v.voice));
          v.size = vo["size"] | 0;
          v.version = vo["version"] | 0;
          v.format = vo["format"] | 1;
          if (!v.file[0]) v.present = false;
        }
        if (take()) { vinfo = v; seenManifest = true; give(); }
      }
      if (versionNewer(ver, MWC_VERSION)) {
        if (strcmp(ver, st.latest) != 0) triedInstallFor = false;
        setState(State::Available);
        LOGI("update: v%s available (running v%s)", ver, MWC_VERSION);
        return true;
      }
      setState(State::UpToDate);
      LOGI("update: up to date (latest v%s)", ver);
      return true;
    }

    bool install(const AppConfig& cfg) {
      Status cur = status();
      if (cur.state != State::Available && cur.state != State::Error && cur.state != State::UpToDate) return false;
      if (!cur.file[0]) { setState(State::Error, "no file to install"); return false; }
      String url = baseUrl + cur.file;
      LOGI("update: downloading %s", url.c_str());
      setState(State::Downloading);
      setProgress(0);
      renderer::setOta(true, 0);
      http_util::Options opt;
      opt.userAgent = MWC_USER_AGENT_NAME "/" MWC_VERSION;
      opt.accept = "application/octet-stream";
      opt.timeoutMs = 30000;
      String err;
      int code = 0;
      bool ok = http_util::get(url, opt, [&](Stream& s, int len) -> bool {
        if (len <= 0) { err = "unknown download size"; return false; }
        if (cur.size && (uint32_t)len != cur.size) { err = "size differs from manifest"; return false; }
        if (!Update.begin((size_t)len, U_FLASH)) { err = Update.errorString(); return false; }
        if (md5[0]) Update.setMD5(md5);
        uint8_t* buf = (uint8_t*)heap_caps_malloc(CHUNK, MALLOC_CAP_8BIT);
        if (!buf) { err = "no memory"; Update.abort(); return false; }
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
          if (Update.write(buf, n) != (size_t)n) { err = Update.errorString(); break; }
          got += n;
          uint8_t pct = (uint8_t)((uint64_t)got * 100 / len);
          setProgress(pct);
          renderer::setOta(true, pct > 99 ? 99 : pct);
        }
        free(buf);
        if (got < (size_t)len) { Update.abort(); return false; }
        setState(State::Installing);
        if (!Update.end(true)) { err = Update.errorString(); return false; }   // verifies the MD5 when one was set
        return true;
      }, err, &code);
      if (!ok) {
        renderer::setOta(false, 0);
        setState(State::Error, err.c_str());
        LOGE("update: install failed: %s", err.c_str());
        nextCheck = millis() + RETRY_MS;
        return false;
      }
      renderer::setOta(true, 100);
      setState(State::Done);
      LOGI("update: v%s installed, rebooting", cur.latest);
      app::requestReboot(1500);
      return true;
    }
  }

  void begin() {
    mtx = xSemaphoreCreateMutex();
    st.state = State::Idle;
    nextCheck = millis() + FIRST_CHECK_MS;
  }

  void requestCheck() { checkRequested = true; net_task::kick(net_task::JOB_UPDATE); }
  void requestInstall() { installRequested = true; net_task::kick(net_task::JOB_UPDATE); }

  bool versionNewer(const char* remote, const char* local) {
    const char* a = remote; const char* b = local;
    for (int i = 0; i < 4; i++) {
      int ra = parsePart(a), lb = parsePart(b);
      if (ra != lb) return ra > lb;
      if (!*a && !*b) break;
    }
    return false;
  }

  void run(const AppConfig& cfg) {
    if (!wifi_mgr::isConnected() || app::rebootPending()) return;
    const uint32_t now = millis();
    if (installRequested) {
      installRequested = false;
      if (status().state != State::Available) check(cfg);
      install(cfg);
      return;
    }
    if (checkRequested || (cfg.update.check && (int32_t)(now - nextCheck) >= 0)) {
      checkRequested = false;
      nextCheck = now + (uint32_t)cfg.update.check_hours * 3600000UL;
      if (!check(cfg)) nextCheck = now + RETRY_MS;
    }
    if (cfg.update.check && cfg.update.auto_install && status().state == State::Available && !triedInstallFor) {
      if (alarmclock::ringing() || alarmclock::timerRunning()) return;     // try again on the next pass
      triedInstallFor = true;
      install(cfg);
    }
  }

  Status status() { Status copy = {}; if (take()) { copy = st; give(); } return copy; }
  VoiceInfo voiceInfo() { VoiceInfo copy; if (take()) { copy = vinfo; give(); } return copy; }
  String manifestBase() { String b; if (take()) { b = baseUrl; give(); } return b; }
  bool manifestSeen() { bool s = false; if (take()) { s = seenManifest; give(); } return s; }

  const char* stateName(State s) {
    switch (s) {
      case State::Checking: return "checking";
      case State::UpToDate: return "up-to-date";
      case State::Available: return "available";
      case State::Downloading: return "downloading";
      case State::Installing: return "installing";
      case State::Done: return "done";
      case State::Error: return "error";
      default: return "idle";
    }
  }
}
