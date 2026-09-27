#include "radar.h"
#include <PNGdec.h>
#include <new>
#include <math.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "http_util.h"
#include "net_task.h"
#include "wifi_manager.h"
#include "util/log.h"
#include "version.h"

namespace radar {
  namespace {
    constexpr const char* BASE = "https://mesonet.agron.iastate.edu/cgi-bin/wms/nexrad/n0r.cgi?SERVICE=WMS&VERSION=1.1.1&REQUEST=GetMap&STYLES=&SRS=EPSG:4326&FORMAT=image/png&TRANSPARENT=FALSE&BGCOLOR=0x000000";
    // NOAA nowCOAST: the MRMS (Multi-Radar Multi-Sensor) base reflectivity mosaic, quality controlled, so birds, insects,
    // ground and sea clutter are removed; WMS 1.3.0 with a time dimension (nearestValue), CRS:84 keeps lon/lat order
    constexpr const char* NOWCOAST = "https://nowcoast.noaa.gov/geoserver/weather_radar/wms?SERVICE=WMS&VERSION=1.3.0&REQUEST=GetMap&LAYERS=base_reflectivity_mosaic&STYLES=&CRS=CRS:84&FORMAT=image/png&TRANSPARENT=FALSE&BGCOLOR=0x000000";
    constexpr const char* GIBS = "https://gibs.earthdata.nasa.gov/wms/epsg4326/best/wms.cgi?SERVICE=WMS&REQUEST=GetMap&VERSION=1.1.1&STYLES=&SRS=EPSG:4326&FORMAT=image/png&TRANSPARENT=TRUE";
    constexpr size_t MAX_PNG = 24 * 1024;
    constexpr uint32_t DUP_RETRY_MS = 60000UL;
    constexpr size_t FRAME_PX = (size_t)W * H;

    SemaphoreHandle_t mtx = nullptr;
    uint16_t* frames = nullptr;                 // MAX_FRAMES * FRAME_PX, oldest first
    uint32_t frameT[MAX_FRAMES];                // millis() the composite is valid for (approximate)
    uint8_t count = 0;
    uint8_t* pngBuf = nullptr;
    uint16_t* work = nullptr;                   // decode target
    PNG* png = nullptr;
    Status st = {};
    uint32_t nextFetch = 0;
    volatile bool refreshRequested = false, resetRequested = false;
    int pngW = 0, pngH = 0;
    uint8_t* base = nullptr;                    // W*H mask, BASE_* bits
    uint8_t baseMode = 0;                       // cfg.radar.base_map the mask was built for
    bool baseValid = false;
    uint32_t baseNext = 0;
    uint8_t baseFails = 0;
    uint8_t baseBit = 0;                        // which layer the decoder is filling
    uint8_t baseState = 0;

    bool take() { return mtx && xSemaphoreTake(mtx, pdMS_TO_TICKS(100)) == pdTRUE; }
    void give() { xSemaphoreGive(mtx); }

    int drawLine(PNGDRAW* d) {   // returns 1 to keep decoding
      // nearest-neighbour into the 64x32 work frame in case the server returns another size
      static uint16_t* line = nullptr;   // PSRAM
      if (!line) line = (uint16_t*)heap_caps_malloc(1024 * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (!line || d->iWidth > 1024) return 0;
      PNG* p = (PNG*)d->pUser;
      p->getLineAsRGB565(d, line, PNG_RGB565_LITTLE_ENDIAN, 0xffffffff);
      if (pngW == W && pngH == H) { if (d->y < H) memcpy(work + d->y * W, line, W * 2); return 1; }
      int y0 = (int)((int32_t)d->y * H / pngH), y1 = (int)((int32_t)(d->y + 1) * H / pngH);
      if (y1 <= y0) y1 = y0 + 1;
      for (int y = y0; y < y1 && y < H; y++)
        for (int x = 0; x < W; x++) work[y * W + x] = line[(int)((int32_t)x * pngW / W)];
      return 1;
    }

    // GIBS base layers: OSM_Land_Water_Map is two flat greys (land 75, water 128); Coastlines_15m an anti-aliased
    // light line with alpha. Fills the base mask straight from the decoder's raw RGBA / RGB line.
    int drawBaseLine(PNGDRAW* d) {
      if (d->y >= H || d->iWidth < 1) return 1;
      const uint8_t* px = d->pPixels;
      const int bpp = d->iPixelType == 6 ? 4 : (d->iPixelType == 2 ? 3 : 0);
      if (!bpp || d->iBpp != 8) return 0;
      for (int x = 0; x < W; x++) {
        int sx = pngW == W ? x : (int)((int32_t)x * pngW / W);
        const uint8_t* p = px + sx * bpp;
        uint8_t lum = (uint8_t)(((int)p[0] + p[1] + p[2]) / 3), a = bpp == 4 ? p[3] : 255;
        uint8_t& m = base[d->y * W + x];
        if (baseBit == BASE_COAST) { if (a >= 60 && lum >= 60) m |= BASE_COAST; }
        else if (a >= 128) m |= lum >= 100 ? BASE_WATER : BASE_LAND;
      }
      return 1;
    }

    String bbox(const AppConfig& cfg) {
      const float r = (float)cfg.radar.radius_km;
      const float lat = cfg.location.lat, lon = cfg.location.lon;
      const float dlat = r / 111.32f, dlon = r / (111.32f * cosf(lat * (float)M_PI / 180.0f));
      char b[96];
      snprintf(b, sizeof(b), "%.4f,%.4f,%.4f,%.4f", lon - dlon, lat - dlat / 2, lon + dlon, lat + dlat / 2);   // 2r wide, r tall
      return String(b);
    }

    bool fetchPng(const String& u, String& err, size_t& got) {
      http_util::Options opt;
      opt.userAgent = MWC_USER_AGENT_NAME "/" MWC_VERSION;
      opt.accept = "image/png";
      opt.timeoutMs = 20000;
      int code = 0;
      bool ok = http_util::get(u, opt, [&](Stream& s, int len) -> bool { return http_util::readBody(s, len, pngBuf, MAX_PNG, got, err); }, err, &code);
      if (!ok) return false;
      if (got < 8 || memcmp(pngBuf, "\x89PNG", 4) != 0) { err = "not a PNG (service error?)"; return false; }
      return true;
    }

    bool fetchBaseLayer(const AppConfig& cfg, const char* layer, uint8_t bit, String& err) {
      String u = GIBS;
      u += "&LAYERS="; u += layer;
      u += "&BBOX="; u += bbox(cfg);
      u += "&WIDTH="; u += W; u += "&HEIGHT="; u += H;
      size_t got = 0;
      if (!fetchPng(u, err, got)) return false;
      baseBit = bit;
      int rc = png->openRAM(pngBuf, (int)got, drawBaseLine);
      if (rc != PNG_SUCCESS) { err = "png open failed"; return false; }
      pngW = png->getWidth(); pngH = png->getHeight();
      rc = png->decode(png, 0);
      png->close();
      if (rc != PNG_SUCCESS) { err = String("png decode error ") + rc; return false; }
      return true;
    }

    void fetchBase(const AppConfig& cfg, uint32_t now) {
      const uint8_t mode = cfg.radar.base_map & 3;
      if (!base || !mode) { baseValid = false; baseState = 0; return; }
      if (baseValid && baseMode == mode) return;
      if ((int32_t)(now - baseNext) < 0) return;
      baseState = 1;
      String err;
      memset(base, 0, FRAME_PX);
      bool ok = true;
      if ((mode & 2) && !fetchBaseLayer(cfg, "OSM_Land_Water_Map", BASE_LAND, err)) ok = false;
      if (ok && (mode & 1) && !fetchBaseLayer(cfg, "Coastlines_15m", BASE_COAST, err)) ok = false;
      if (ok) { baseValid = true; baseMode = mode; baseFails = 0; baseState = 2; LOGI("radar: base map loaded (%s)", mode == 3 ? "coast + water" : mode == 2 ? "water" : "coast"); }
      else {
        if (baseFails < 10) baseFails++;
        baseNext = now + min<uint32_t>(5UL * 60000UL * baseFails, 60UL * 60000UL);
        baseState = 3;
        LOGW("radar: base map: %s", err.c_str());
      }
    }

    // minutesAgo 0 = the newest composite. Mesonet keeps fixed -m05m..-m50m layers; nowCOAST takes an ISO time and
    // snaps to the nearest scan it has.
    String url(const AppConfig& cfg, uint8_t minutesAgo) {
      String u;
      if (cfg.radar.source == 1) {
        u = BASE;
        u += "&LAYERS=nexrad-n0r";
        if (minutesAgo) { char sfx[8]; snprintf(sfx, sizeof(sfx), "-m%02um", minutesAgo); u += sfx; }
      } else {
        u = NOWCOAST;
        if (minutesAgo) {
          time_t t = time(nullptr) - (time_t)minutesAgo * 60;
          struct tm g; gmtime_r(&t, &g);
          char iso[24]; strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", &g);
          u += "&TIME="; u += iso;
        }
      }
      u += "&BBOX="; u += bbox(cfg);
      u += "&WIDTH="; u += W; u += "&HEIGHT="; u += H;
      return u;
    }

    bool fetchLayer(const AppConfig& cfg, uint8_t minutesAgo, String& err) {
      size_t got = 0;
      if (!fetchPng(url(cfg, minutesAgo), err, got)) return false;   // HTTP/1.0: body ends when the server closes
      memset(work, 0, FRAME_PX * 2);
      int rc = png->openRAM(pngBuf, (int)got, drawLine);
      if (rc != PNG_SUCCESS) { err = "png open failed"; return false; }
      pngW = png->getWidth(); pngH = png->getHeight();
      rc = png->decode(png, 0);
      png->close();
      if (rc != PNG_SUCCESS) { err = String("png decode error ") + rc; return false; }
      return true;
    }

    void analyse() {   // newest frame: echo statistics (caller holds the mutex)
      st.echo_near = false; st.echo_pct = 0;
      if (!count) return;
      const uint16_t* f = frames + (count - 1) * FRAME_PX;
      uint32_t all = 0, near = 0;
      for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        if (!f[y * W + x]) continue;
        all++;
        if (x >= W / 3 && x < 2 * W / 3 && y >= H / 3 && y < 2 * H / 3) near++;
      }
      st.echo_pct = (uint8_t)(all * 100 / FRAME_PX);
      st.echo_near = near >= 3;
    }

    void push(uint32_t validAt) {   // work -> newest frame
      if (!take()) return;
      if (count == MAX_FRAMES) { memmove(frames, frames + FRAME_PX, (MAX_FRAMES - 1) * FRAME_PX * 2); memmove(frameT, frameT + 1, (MAX_FRAMES - 1) * sizeof(uint32_t)); count--; }
      memcpy(frames + count * FRAME_PX, work, FRAME_PX * 2);
      frameT[count] = validAt;
      count++;
      analyse();
      give();
    }
  }

  void begin() {
    mtx = xSemaphoreCreateMutex();
    frames = (uint16_t*)heap_caps_calloc(MAX_FRAMES * FRAME_PX, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    work = (uint16_t*)heap_caps_malloc(FRAME_PX * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    pngBuf = (uint8_t*)heap_caps_malloc(MAX_PNG, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    base = (uint8_t*)heap_caps_calloc(FRAME_PX, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    void* pm = heap_caps_malloc(sizeof(PNG), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);   // decoder state stays out of internal RAM
    png = pm ? new (pm) PNG() : nullptr;
    if (!frames || !work || !pngBuf || !png) LOGE("radar: no memory");
    nextFetch = millis() + 20000UL;
  }

  void applyConfig() { resetRequested = true; baseValid = false; baseNext = 0; nextFetch = 0; net_task::kick(net_task::JOB_RADAR); }
  void requestRefresh() { refreshRequested = true; net_task::kick(net_task::JOB_RADAR); }

  bool due(const AppConfig& cfg, uint32_t now) {
    if (!cfg.radar.enabled || !frames) return false;
    const uint8_t mode = cfg.radar.base_map & 3;
    const bool baseDue = mode && (!baseValid || baseMode != mode) && (int32_t)(now - baseNext) >= 0;
    return refreshRequested || resetRequested || baseDue || (int32_t)(now - nextFetch) >= 0;
  }

  void run(const AppConfig& cfg) {
    if (!frames || !png) return;
    st.enabled = cfg.radar.enabled;
    if (resetRequested) { resetRequested = false; if (take()) { count = 0; analyse(); give(); } }
    if (!cfg.radar.enabled || !wifi_mgr::isConnected()) return;
    const uint32_t now = millis();
    fetchBase(cfg, now);
    if (!refreshRequested && count && (int32_t)(now - nextFetch) < 0) return;   // only the base map was due
    refreshRequested = false;
    const uint32_t period = (uint32_t)(cfg.radar.refresh_min ? cfg.radar.refresh_min : 5) * 60000UL;
    String err;
    bool ok = true;
    if (cfg.radar.source == 0 && time(nullptr) < 1700000000) { nextFetch = now + 30000UL; return; }   // nowCOAST frames are addressed by time
    if (count == 0) {
      // first fill: the 50..5 minutes-ago frames, then the current composite
      for (int i = 0; i < 10 && ok; i++) {
        const uint8_t ago = (uint8_t)(50 - 5 * i);
        if (!fetchLayer(cfg, ago, err)) { ok = false; break; }
        push(now - (uint32_t)ago * 60000UL);
        delay(400);   // let lwIP release the previous TLS socket before the next one
      }
      if (ok && fetchLayer(cfg, 0, err)) push(now); else ok = false;
    } else {
      if (fetchLayer(cfg, 0, err)) {
        bool same = false;
        if (take()) { same = memcmp(work, frames + (count - 1) * FRAME_PX, FRAME_PX * 2) == 0; give(); }
        if (same) { nextFetch = now + DUP_RETRY_MS; if (take()) { st.last_ok_ms = now; give(); } return; }   // composite not updated yet
        push(now);
      } else ok = false;
    }
    if (take()) {
      if (ok) { st.last_ok_ms = now; st.fails = 0; st.err[0] = '\0'; }
      else { st.last_err_ms = now; if (st.fails < 10) st.fails++; strlcpy(st.err, err.c_str(), sizeof(st.err)); }
      st.frames = count;
      give();
    }
    if (ok) { nextFetch = now + period; LOGI("radar: %u frames, echoes %u%%%s", count, st.echo_pct, st.echo_near ? " nearby" : ""); }
    else { nextFetch = now + min<uint32_t>(period * st.fails, 30 * 60000UL); LOGW("radar: %s (retry in %lu min)", err.c_str(), (unsigned long)((nextFetch - now) / 60000UL)); }
  }

  Status status() { Status c = {}; if (take()) { c = st; c.frames = count; c.base_state = baseState; give(); } return c; }
  uint8_t frameCount() { return count; }
  int32_t frameAgeMin(uint8_t i) {
    int32_t a = -1;
    if (take()) { if (i < count) a = (int32_t)((millis() - frameT[i]) / 60000UL); give(); }
    return a;
  }
  bool copyFrame(uint8_t i, uint16_t* out) {
    if (!take()) return false;
    bool ok = i < count;
    if (ok) memcpy(out, frames + i * FRAME_PX, FRAME_PX * 2);
    give();
    return ok;
  }
  size_t copyFrameBytes(uint8_t i, uint8_t* out, size_t offset, size_t maxLen) {
    if (!take()) return 0;
    size_t n = 0;
    if (i < count && offset < FRAME_PX * 2) {
      n = min(maxLen, FRAME_PX * 2 - offset);
      memcpy(out, (const uint8_t*)(frames + i * FRAME_PX) + offset, n);
    }
    give();
    return n;
  }
  bool echoNearby() { bool e = false; if (take()) { e = count > 0 && st.echo_near; give(); } return e; }
  bool copyBase(uint8_t* out) {
    if (!base || !baseValid) return false;
    if (!take()) return false;
    memcpy(out, base, FRAME_PX);
    give();
    return true;
  }
  size_t copyBaseBytes(uint8_t* out, size_t offset, size_t maxLen) {
    if (!base || !baseValid || offset >= FRAME_PX) return 0;
    if (!take()) return 0;
    size_t n = min(maxLen, FRAME_PX - offset);
    memcpy(out, base + offset, n);
    give();
    return n;
  }
}
