// 128x128 ST7735 backend (Spotpear ESP32-C3 1.44-inch "mini TV"). Implements the panel:: interface from panel.h.
//
// Like the 4-inch LCD, the screen stacks two 64x32 canvases into one 64x64 picture: the clock on top and the full
// screens (forecast, hourly graph, world clock, radar) below. At 2 screen pixels per canvas pixel that is exactly
// 128x128. Turning the picture (panel.rotation) is done here in software; the panel keeps the vendor's orientation.
//
// The backlight is wired straight to the supply, so brightness scales the pixel colours instead, from a floor so the
// LED-tuned night level stays readable. Only the rectangle that changed is sent, over 40 MHz SPI.
#include "board.h"
#if defined(MWC_BOARD_C3TV)
#include "panel.h"
#include <SPI.h>
#include "layout.h"
#include "pins.h"
#include "util/log.h"

namespace panel {
  namespace {
    constexpr int SCREEN = 128;
    constexpr int SCALE = 2;
    constexpr uint32_t SPI_HZ = 40000000;
    // green tab 3 in the vendor's rotation 2: RGB/BGR bit set, column offset 2, row offset 1
    constexpr uint8_t MADCTL = 0x08;
    constexpr uint8_t COL_OFS = 2, ROW_OFS = 1;
    constexpr uint8_t BRI_FLOOR = 48;           // scale for level 1; 255 = full colour

    SPISettings spiCfg(SPI_HZ, MSBFIRST, SPI_MODE0);
    uint16_t* prevFrame = nullptr;              // last canvas pixels drawn (64x64), before brightness scaling
    constexpr int16_t FW = layout::W, FH = layout::H * 2;
    uint8_t rot = 0;
    uint8_t maxBri = 255, curBri = 255, scale = 255;
    bool isValid = false, fullFrame = true;

    // ---- SPI ----
    void cmd(uint8_t c) {
      digitalWrite(pins::TFT_DC, LOW);
      digitalWrite(pins::TFT_CS, LOW);
      SPI.beginTransaction(spiCfg);
      SPI.write(c);
      SPI.endTransaction();
      digitalWrite(pins::TFT_CS, HIGH);
    }
    void data(const uint8_t* d, size_t n) {
      digitalWrite(pins::TFT_DC, HIGH);
      digitalWrite(pins::TFT_CS, LOW);
      SPI.beginTransaction(spiCfg);
      SPI.writeBytes(d, n);
      SPI.endTransaction();
      digitalWrite(pins::TFT_CS, HIGH);
    }
    void cmdData(uint8_t c, std::initializer_list<uint8_t> d) {
      cmd(c);
      if (d.size()) data(d.begin(), d.size());
    }

    // ST7735R init for the green tab panels (TFT_eSPI Rcmd1 + Rcmd2green + Rcmd3, as the vendor demo sends it)
    void panelInit() {
      if (pins::TFT_RST >= 0) {
        pinMode(pins::TFT_RST, OUTPUT);
        digitalWrite(pins::TFT_RST, HIGH); delay(5);
        digitalWrite(pins::TFT_RST, LOW);  delay(20);
        digitalWrite(pins::TFT_RST, HIGH); delay(150);
      }
      cmd(0x01); delay(150);                                       // software reset
      cmd(0x11); delay(500);                                       // sleep out
      cmdData(0xB1, { 0x01, 0x2C, 0x2D });                         // frame rate, normal mode
      cmdData(0xB2, { 0x01, 0x2C, 0x2D });                         // idle mode
      cmdData(0xB3, { 0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D });       // partial mode
      cmdData(0xB4, { 0x07 });                                     // no inversion
      cmdData(0xC0, { 0xA2, 0x02, 0x84 });                         // power control 1..5
      cmdData(0xC1, { 0xC5 });
      cmdData(0xC2, { 0x0A, 0x00 });
      cmdData(0xC3, { 0x8A, 0x2A });
      cmdData(0xC4, { 0x8A, 0xEE });
      cmdData(0xC5, { 0x0E });                                     // VCOM
      cmd(0x20);                                                   // inversion off (the vendor demo turns it off)
      cmdData(0x3A, { 0x05 });                                     // 16-bit colour
      cmdData(0xE0, { 0x02, 0x1C, 0x07, 0x12, 0x37, 0x32, 0x29, 0x2D, 0x29, 0x25, 0x2B, 0x39, 0x00, 0x01, 0x03, 0x10 });
      cmdData(0xE1, { 0x03, 0x1D, 0x07, 0x06, 0x2E, 0x2C, 0x29, 0x2D, 0x2E, 0x2E, 0x37, 0x3F, 0x00, 0x00, 0x02, 0x10 });
      cmdData(0x36, { MADCTL });
      cmd(0x13); delay(10);                                        // normal display on
      cmd(0x29); delay(100);                                       // display on
    }

    void window(int x0, int y0, int x1, int y1) {
      const uint16_t c0 = x0 + COL_OFS, c1 = x1 + COL_OFS, r0 = y0 + ROW_OFS, r1 = y1 + ROW_OFS;
      cmdData(0x2A, { (uint8_t)(c0 >> 8), (uint8_t)c0, (uint8_t)(c1 >> 8), (uint8_t)c1 });
      cmdData(0x2B, { (uint8_t)(r0 >> 8), (uint8_t)r0, (uint8_t)(r1 >> 8), (uint8_t)r1 });
      cmd(0x2C);
    }

    inline uint16_t dim(uint16_t v) {
      if (scale == 255 || !v) return v;
      const uint16_t r = ((v >> 11) & 0x1F) * scale / 255, g = ((v >> 5) & 0x3F) * scale / 255, b = (v & 0x1F) * scale / 255;
      return (uint16_t)((r << 11) | (g << 5) | b);
    }

    // canvas pixel shown in screen cell (cx, cy) after rotation; the picture is square, so all four turns fit
    inline void cellToCanvas(int cx, int cy, int& x, int& y) {
      switch (rot) {
        case 1:  x = cy;          y = FH - 1 - cx; break;   // 90 degrees clockwise
        case 2:  x = FW - 1 - cx; y = FH - 1 - cy; break;
        case 3:  x = FW - 1 - cy; y = cx;          break;
        default: x = cx;          y = cy;          break;
      }
    }
    inline void canvasToCell(int x, int y, int& cx, int& cy) {
      switch (rot) {
        case 1:  cx = FH - 1 - y; cy = x;          break;
        case 2:  cx = FW - 1 - x; cy = FH - 1 - y; break;
        case 3:  cx = y;          cy = FW - 1 - x; break;
        default: cx = x;          cy = y;          break;
      }
    }

    // sends cells [cx0..cx1] x [cy0..cy1] from prevFrame
    void pushCells(int cx0, int cy0, int cx1, int cy1) {
      static uint8_t line[SCREEN * 2];
      window(cx0 * SCALE, cy0 * SCALE, cx1 * SCALE + SCALE - 1, cy1 * SCALE + SCALE - 1);
      digitalWrite(pins::TFT_DC, HIGH);
      digitalWrite(pins::TFT_CS, LOW);
      SPI.beginTransaction(spiCfg);
      for (int cy = cy0; cy <= cy1; cy++) {
        size_t n = 0;
        for (int cx = cx0; cx <= cx1; cx++) {
          int x, y;
          cellToCanvas(cx, cy, x, y);
          const uint16_t v = dim(prevFrame[y * FW + x]);
          for (int k = 0; k < SCALE; k++) { line[n++] = v >> 8; line[n++] = v & 0xFF; }
        }
        for (int k = 0; k < SCALE; k++) SPI.writeBytes(line, n);
      }
      SPI.endTransaction();
      digitalWrite(pins::TFT_CS, HIGH);
    }

    // copies the canvases into prevFrame and sends the screen rectangle that changed
    void update(const Canvas* top, const Canvas* bottom) {
      int minX = FW, minY = FH, maxX = -1, maxY = -1;
      const bool force = fullFrame;
      fullFrame = false;
      for (int y = 0; y < FH; y++) {
        const Canvas* c = y < layout::H ? top : bottom;
        const int cyy = y < layout::H ? y : y - layout::H;
        const uint16_t* row = c && cyy < c->height() ? c->getBuffer() + cyy * c->width() : nullptr;
        uint16_t* prow = prevFrame + y * FW;
        for (int x = 0; x < FW; x++) {
          const uint16_t v = row && x < c->width() ? row[x] : 0;
          if (!force && v == prow[x]) continue;
          prow[x] = v;
          if (x < minX) minX = x;
          if (x > maxX) maxX = x;
          if (y < minY) minY = y;
          if (y > maxY) maxY = y;
        }
      }
      if (force) { pushCells(0, 0, FW - 1, FH - 1); return; }
      if (maxX < 0) return;
      int ax, ay, bx, by;
      canvasToCell(minX, minY, ax, ay);
      canvasToCell(maxX, maxY, bx, by);
      pushCells(min(ax, bx), min(ay, by), max(ax, bx), max(ay, by));
    }
  }

  bool begin(const PanelConfig& pc, uint8_t brightness, float gamma) {
    (void)gamma;
    if (isValid) return true;
    maxBri = pc.max_brightness;
    rot = pc.rotation & 3;
    prevFrame = (uint16_t*)calloc(FW * FH, sizeof(uint16_t));
    if (!prevFrame) { LOGE("tft: no memory for frame copy"); return false; }
    pinMode(pins::TFT_CS, OUTPUT); digitalWrite(pins::TFT_CS, HIGH);
    pinMode(pins::TFT_DC, OUTPUT); digitalWrite(pins::TFT_DC, HIGH);
    SPI.begin(pins::TFT_SCLK, -1, pins::TFT_MOSI, -1);
    panelInit();
    isValid = true;
    setBrightness(brightness);
    fullFrame = true;
    update(nullptr, nullptr);                  // clear to black
    LOGI("tft: ST7735 128x128, 64x64 canvas at %d px per pixel, rotation %u", SCALE, rot * 90);
    return true;
  }

  bool valid() { return isValid; }

  void setBrightness(uint8_t level) {
    curBri = level > maxBri ? maxBri : level;
    const uint8_t s = curBri ? (uint8_t)(BRI_FLOOR + (uint32_t)curBri * (255 - BRI_FLOOR) / 255) : 0;
    if (s != scale) { scale = s; fullFrame = true; }   // every pixel changes: resend on the next frame
  }

  uint8_t brightness() { return curBri; }
  void setGamma(float) { fullFrame = true; }
  void setLatchBlanking(uint8_t) {}

  void setRotation(uint8_t quarterTurns) {
    quarterTurns &= 3;
    if (quarterTurns == rot) return;
    rot = quarterTurns;
    fullFrame = true;
    LOGI("tft: rotation %u", rot * 90);
  }

  void present(const Canvas& c, bool force) {
    if (!isValid) return;
    if (force) fullFrame = true;
    update(&c, nullptr);
  }

  void presentStacked(const Canvas& top, const Canvas& bottom) {
    if (!isValid) return;
    update(&top, &bottom);
  }

  bool stacked() { return isValid; }
  int refreshRateHz() { return isValid ? 60 : 0; }      // the controller refreshes from its own RAM
  const char* driverName() { return "ST7735 TFT"; }
}
#endif
