#include "config.h"
#include <LittleFS.h>
#include "util/log.h"
#include "util/psram_alloc.h"

AppConfig g_cfg;

namespace {
  constexpr const char* CFG_PATH = "/config.json";
  constexpr const char* CFG_TMP = "/config.tmp";
}

bool config_load() {
  AppConfig fresh;
  g_cfg = fresh;
  File f = LittleFS.open(CFG_PATH, "r");
  if (!f) { LOGW("config: no %s, using defaults", CFG_PATH); return false; }
  JsonDocument doc(psramAllocator());
  DeserializationError e = deserializeJson(doc, f);
  f.close();
  if (e) { LOGE("config: parse error %s, using defaults", e.c_str()); return false; }
  uint32_t changed = 0;
  String err;
  AppConfig merged;
  if (!config_from_json(doc.as<JsonObjectConst>(), merged, changed, err)) {
    LOGE("config: invalid content (%s), using defaults", err.c_str());
    return false;
  }
  g_cfg = merged;
  LOGI("config: loaded");
  return true;
}

bool config_save(const AppConfig& c) {
  JsonDocument doc(psramAllocator());
  JsonObject root = doc.to<JsonObject>();
  config_to_json(c, root, false);
  File f = LittleFS.open(CFG_TMP, "w");
  if (!f) { LOGE("config: cannot open %s for writing", CFG_TMP); return false; }
  size_t n = serializeJsonPretty(doc, f);
  f.close();
  if (n == 0) { LOGE("config: serialize failed"); LittleFS.remove(CFG_TMP); return false; }
  LittleFS.remove(CFG_PATH);
  if (!LittleFS.rename(CFG_TMP, CFG_PATH)) { LOGE("config: rename failed"); return false; }
  LOGI("config: saved (%u bytes)", (unsigned)n);
  return true;
}

void config_factory_reset() {
  LOGW("config: factory reset");
  LittleFS.remove(CFG_PATH);
  LittleFS.remove(CFG_TMP);
  delay(200);
  ESP.restart();
}
