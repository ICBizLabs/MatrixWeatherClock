#include "frame_snapshot.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace frame_snapshot {
  namespace {
    SemaphoreHandle_t mtx = nullptr;
    uint16_t* buf = nullptr;
    uint16_t W = 0, H = 0;
  }

  void begin(uint16_t w, uint16_t h) {
    W = w; H = h;
    mtx = xSemaphoreCreateMutex();
    buf = (uint16_t*)calloc(W * H, sizeof(uint16_t));
  }

  void update(const uint16_t* pixels, uint16_t y0, uint16_t rows) {
    if (!rows) rows = H;
    if (y0 >= H) return;
    if (rows > H - y0) rows = H - y0;
    if (!buf || !mtx || xSemaphoreTake(mtx, 0) != pdTRUE) return;   // skip a frame rather than stall rendering
    memcpy(buf + (size_t)y0 * W, pixels, (size_t)W * rows * sizeof(uint16_t));
    xSemaphoreGive(mtx);
  }

  size_t bytes() { return (size_t)W * H * 2; }
  uint16_t width() { return W; }
  uint16_t height() { return H; }

  size_t copy(uint8_t* out, size_t offset, size_t maxLen) {
    size_t total = bytes();
    if (!buf || offset >= total) return 0;
    size_t n = total - offset;
    if (n > maxLen) n = maxLen;
    if (mtx && xSemaphoreTake(mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
      memcpy(out, (const uint8_t*)buf + offset, n);
      xSemaphoreGive(mtx);
      return n;
    }
    return 0;
  }
}
