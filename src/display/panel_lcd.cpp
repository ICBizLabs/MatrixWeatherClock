// 480x480 ST7701S LCD backend (Guition ESP32-4848S040). Implements the panel:: interface from panel.h.
//
// The clock renders into 64x32 canvases. The LCD stacks two of them, the clock on top and the full screens
// (forecast, hourly graph, world clock, radar) below, into a 64x64 picture. Each canvas pixel becomes a 7x7 cell:
// a 6x6 dot with its corners trimmed and a one-pixel gap, so the screen keeps the look of an LED matrix. The
// 448x448 image sits in the middle of the panel and can be turned in quarter steps (panel.rotation).
//
// The ST7701S is configured once over a bit-banged 3-wire SPI link (9-bit words: D/C bit, then 8 data bits),
// then fed continuously over the ESP32-S3 RGB interface from a PSRAM frame buffer through DRAM bounce buffers.
// The init table is the one this board's stock firmware sends (same as Arduino_GFX st7701_type9_init_operations).
#include "board.h"
#if defined(MWC_BOARD_LCD4848)
#include "panel.h"
#include <esp_lcd_panel_rgb.h>
#include <esp_lcd_panel_ops.h>
#include <esp_heap_caps.h>
#include "layout.h"
#include "pins.h"
#include "util/log.h"

namespace panel {
  namespace {
    constexpr int LCD_W = 480, LCD_H = 480;
    constexpr int CELL = 7;                    // screen pixels per canvas pixel: a 6-pixel dot and a 1-pixel gap
    constexpr uint32_t PCLK_HZ = 12000000;
    constexpr int HSYNC_PW = 8, HSYNC_BP = 50, HSYNC_FP = 10;
    constexpr int VSYNC_PW = 8, VSYNC_BP = 20, VSYNC_FP = 10;
    constexpr uint32_t BL_PWM_HZ = 20000;

    esp_lcd_panel_handle_t rgb = nullptr;
    uint16_t* fb = nullptr;                    // the driver's PSRAM frame buffer, RGB565
    uint16_t* prevFrame = nullptr;             // last presented canvas
    int16_t frameW = 0, frameH = 0;
    uint8_t rot = 0;
    uint8_t maxBri = 255, curBri = 0;
    bool isValid = false;
    bool fullFrame = true;
    bool backlightOn = false;

    // ---- ST7701S 3-wire SPI ----
    inline void spiBit(bool b) {
      digitalWrite(pins::LCD_SCK, LOW);
      digitalWrite(pins::LCD_SDA, b ? HIGH : LOW);
      delayMicroseconds(1);
      digitalWrite(pins::LCD_SCK, HIGH);       // the panel samples on the rising edge
      delayMicroseconds(1);
    }
    void spiWord(bool isData, uint8_t v) {
      spiBit(isData);
      for (int i = 7; i >= 0; i--) spiBit((v >> i) & 1);
    }
    void sendCmd(uint8_t cmd, const uint8_t* data = nullptr, uint8_t n = 0) {
      digitalWrite(pins::LCD_CS, LOW);
      spiWord(false, cmd);
      for (uint8_t i = 0; i < n; i++) spiWord(true, data[i]);
      digitalWrite(pins::LCD_SCK, LOW);
      digitalWrite(pins::LCD_CS, HIGH);
      delayMicroseconds(2);
    }

    struct InitCmd { uint8_t cmd; uint8_t n; uint8_t data[16]; uint16_t delayMs; };
    const InitCmd INIT[] = {
      { 0xFF, 5, { 0x77, 0x01, 0x00, 0x00, 0x10 }, 0 },             // command 2, bank 0
      { 0xC0, 2, { 0x3B, 0x00 }, 0 },
      { 0xC1, 2, { 0x0D, 0x02 }, 0 },
      { 0xC2, 2, { 0x31, 0x05 }, 0 },
      { 0xCD, 1, { 0x00 }, 0 },
      { 0xB0, 16, { 0x00, 0x11, 0x18, 0x0E, 0x11, 0x06, 0x07, 0x08, 0x07, 0x22, 0x04, 0x12, 0x0F, 0xAA, 0x31, 0x18 }, 0 },  // positive gamma
      { 0xB1, 16, { 0x00, 0x11, 0x19, 0x0E, 0x12, 0x07, 0x08, 0x08, 0x08, 0x22, 0x04, 0x11, 0x11, 0xA9, 0x32, 0x18 }, 0 },  // negative gamma
      { 0xFF, 5, { 0x77, 0x01, 0x00, 0x00, 0x11 }, 0 },             // command 2, bank 1
      { 0xB0, 1, { 0x60 }, 0 },
      { 0xB1, 1, { 0x32 }, 0 },
      { 0xB2, 1, { 0x07 }, 0 },
      { 0xB3, 1, { 0x80 }, 0 },
      { 0xB5, 1, { 0x49 }, 0 },
      { 0xB7, 1, { 0x85 }, 0 },
      { 0xB8, 1, { 0x21 }, 0 },
      { 0xC1, 1, { 0x78 }, 0 },
      { 0xC2, 1, { 0x78 }, 0 },
      { 0xE0, 3, { 0x00, 0x1B, 0x02 }, 0 },
      { 0xE1, 11, { 0x08, 0xA0, 0x00, 0x00, 0x07, 0xA0, 0x00, 0x00, 0x00, 0x44, 0x44 }, 0 },
      { 0xE2, 12, { 0x11, 0x11, 0x44, 0x44, 0xED, 0xA0, 0x00, 0x00, 0xEC, 0xA0, 0x00, 0x00 }, 0 },
      { 0xE3, 4, { 0x00, 0x00, 0x11, 0x11 }, 0 },
      { 0xE4, 2, { 0x44, 0x44 }, 0 },
      { 0xE5, 16, { 0x0A, 0xE9, 0xD8, 0xA0, 0x0C, 0xEB, 0xD8, 0xA0, 0x0E, 0xED, 0xD8, 0xA0, 0x10, 0xEF, 0xD8, 0xA0 }, 0 },
      { 0xE6, 4, { 0x00, 0x00, 0x11, 0x11 }, 0 },
      { 0xE7, 2, { 0x44, 0x44 }, 0 },
      { 0xE8, 16, { 0x09, 0xE8, 0xD8, 0xA0, 0x0B, 0xEA, 0xD8, 0xA0, 0x0D, 0xEC, 0xD8, 0xA0, 0x0F, 0xEE, 0xD8, 0xA0 }, 0 },
      { 0xEB, 7, { 0x02, 0x00, 0xE4, 0xE4, 0x88, 0x00, 0x40 }, 0 },
      { 0xEC, 2, { 0x3C, 0x00 }, 0 },
      { 0xED, 16, { 0xAB, 0x89, 0x76, 0x54, 0x02, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x20, 0x45, 0x67, 0x98, 0xBA }, 0 },
      { 0xFF, 5, { 0x77, 0x01, 0x00, 0x00, 0x13 }, 0 },             // command 2, bank 3: VAP and VAN
      { 0xE5, 1, { 0xE4 }, 0 },
      { 0xFF, 5, { 0x77, 0x01, 0x00, 0x00, 0x00 }, 0 },             // back to command 1
      { 0x3A, 1, { 0x60 }, 0 },                                     // COLMOD: 18-bit RGB interface format
      { 0x11, 0, {}, 120 },                                         // sleep out
      { 0x29, 0, {}, 20 },                                          // display on
    };

    void panelInit() {
      pinMode(pins::LCD_CS, OUTPUT);  digitalWrite(pins::LCD_CS, HIGH);
      pinMode(pins::LCD_SCK, OUTPUT); digitalWrite(pins::LCD_SCK, LOW);
      pinMode(pins::LCD_SDA, OUTPUT); digitalWrite(pins::LCD_SDA, LOW);
      delay(5);
      sendCmd(0x01);                           // software reset (the board has no reset line)
      delay(120);
      for (const InitCmd& c : INIT) {
        sendCmd(c.cmd, c.data, c.n);
        if (c.delayMs) delay(c.delayMs);
      }
    }

    // Brightness levels are tuned for LED panels, where 8 of 255 is still clearly lit. A backlight at 3% duty is
    // close to black, so any non-zero level starts from a floor; 0 still turns the backlight off.
    constexpr uint8_t BL_FLOOR = 16;
    void backlight(uint8_t level) {
      const uint32_t duty = level ? BL_FLOOR + (uint32_t)level * (255 - BL_FLOOR) / 255 : 0;
      ledcWrite(pins::LCD_BL, duty);
    }

    // ---- drawing ----
    // dot mask for one 7x7 cell: a 6x6 square with its corners trimmed, then a one-pixel gap
    inline bool dotLit(int r, int c) {
      if (r >= CELL - 1 || c >= CELL - 1) return false;
      const bool edgeR = r == 0 || r == CELL - 2, edgeC = c == 0 || c == CELL - 2;
      return !(edgeR && edgeC);
    }

    // top-left screen pixel of canvas pixel (x, y) after rotation; the image is centred on the panel
    void cellOrigin(int16_t x, int16_t y, int& px, int& py) {
      const bool sideways = rot & 1;
      const int imgW = (sideways ? frameH : frameW) * CELL, imgH = (sideways ? frameW : frameH) * CELL;
      const int ox = (LCD_W - imgW) / 2, oy = (LCD_H - imgH) / 2;
      int cx, cy;
      switch (rot) {
        case 1:  cx = frameH - 1 - y; cy = x; break;                   // 90 degrees clockwise
        case 2:  cx = frameW - 1 - x; cy = frameH - 1 - y; break;
        case 3:  cx = y; cy = frameW - 1 - x; break;
        default: cx = x; cy = y; break;
      }
      px = ox + cx * CELL;
      py = oy + cy * CELL;
    }

    void drawCell(int16_t x, int16_t y, uint16_t color) {
      int px, py;
      cellOrigin(x, y, px, py);
      if (px < 0 || py < 0 || px + CELL > LCD_W || py + CELL > LCD_H) return;
      for (int r = 0; r < CELL; r++) {
        uint16_t* row = fb + (py + r) * LCD_W + px;
        for (int c = 0; c < CELL; c++) row[c] = dotLit(r, c) ? color : 0;
      }
    }
  }

  bool begin(const PanelConfig& pc, uint8_t brightness, float gamma) {
    (void)gamma;                               // the LCD takes sRGB values directly; the LED gamma curve does not apply
    if (rgb) return isValid;
    maxBri = pc.max_brightness;
    rot = pc.rotation & 3;

    ledcAttach(pins::LCD_BL, BL_PWM_HZ, 8);
    backlight(0);                              // dark until the first frame is in place

    panelInit();

    esp_lcd_rgb_panel_config_t cfg = {};
    cfg.clk_src = LCD_CLK_SRC_DEFAULT;
    cfg.timings.pclk_hz = PCLK_HZ;
    cfg.timings.h_res = LCD_W;
    cfg.timings.v_res = LCD_H;
    cfg.timings.hsync_pulse_width = HSYNC_PW;
    cfg.timings.hsync_back_porch = HSYNC_BP;
    cfg.timings.hsync_front_porch = HSYNC_FP;
    cfg.timings.vsync_pulse_width = VSYNC_PW;
    cfg.timings.vsync_back_porch = VSYNC_BP;
    cfg.timings.vsync_front_porch = VSYNC_FP;
    cfg.timings.flags.hsync_idle_low = 0;
    cfg.timings.flags.vsync_idle_low = 0;
    cfg.timings.flags.de_idle_high = 0;
    cfg.timings.flags.pclk_active_neg = 0;
    cfg.timings.flags.pclk_idle_high = 0;
    cfg.data_width = 16;
    cfg.bits_per_pixel = 16;
    cfg.num_fbs = 1;
    cfg.bounce_buffer_size_px = LCD_W * 10;    // DMA reads DRAM; keeps the picture steady while WiFi loads PSRAM
    cfg.dma_burst_size = 64;
    cfg.hsync_gpio_num = pins::LCD_HSYNC;
    cfg.vsync_gpio_num = pins::LCD_VSYNC;
    cfg.de_gpio_num = pins::LCD_DE;
    cfg.pclk_gpio_num = pins::LCD_PCLK;
    cfg.disp_gpio_num = -1;
    // data lines, least significant first: B0..B4, G0..G5, R0..R4 (RGB565)
    for (int i = 0; i < 5; i++) cfg.data_gpio_nums[i] = pins::LCD_B[i];
    for (int i = 0; i < 6; i++) cfg.data_gpio_nums[5 + i] = pins::LCD_G[i];
    for (int i = 0; i < 5; i++) cfg.data_gpio_nums[11 + i] = pins::LCD_R[i];
    cfg.flags.fb_in_psram = 1;

    esp_err_t e = esp_lcd_new_rgb_panel(&cfg, &rgb);
    if (e != ESP_OK) { LOGE("lcd: RGB panel create failed (%s)", esp_err_to_name(e)); rgb = nullptr; return false; }
    if ((e = esp_lcd_panel_reset(rgb)) != ESP_OK || (e = esp_lcd_panel_init(rgb)) != ESP_OK) {
      LOGE("lcd: RGB panel start failed (%s)", esp_err_to_name(e));
      return false;
    }
    void* buf = nullptr;
    if (esp_lcd_rgb_panel_get_frame_buffer(rgb, 1, &buf) != ESP_OK || !buf) { LOGE("lcd: no frame buffer"); return false; }
    fb = (uint16_t*)buf;
    memset(fb, 0, LCD_W * LCD_H * sizeof(uint16_t));

    frameW = layout::W;                        // the HUB75 size settings do not apply: the clock layout is fixed
    frameH = layout::H * 2;                    // top: clock, bottom: full screens
    prevFrame = (uint16_t*)heap_caps_malloc(frameW * frameH * sizeof(uint16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!prevFrame) { LOGE("lcd: no memory for frame copy"); return false; }
    memset(prevFrame, 0, frameW * frameH * sizeof(uint16_t));

    isValid = true;
    fullFrame = true;
    curBri = brightness > maxBri ? maxBri : brightness;
    LOGI("lcd: ST7701 480x480, %dx%d canvas at %d px per pixel, rotation %u, %d Hz", frameW, frameH, CELL, rot * 90, refreshRateHz());
    return true;
  }

  bool valid() { return isValid; }

  void setBrightness(uint8_t level) {
    curBri = level > maxBri ? maxBri : level;
    if (isValid && backlightOn) backlight(curBri);
  }

  uint8_t brightness() { return curBri; }

  void setGamma(float) { fullFrame = true; }

  void setLatchBlanking(uint8_t) {}

  void setRotation(uint8_t quarterTurns) {
    quarterTurns &= 3;
    if (quarterTurns == rot) return;
    rot = quarterTurns;
    if (fb) memset(fb, 0, LCD_W * LCD_H * sizeof(uint16_t));
    fullFrame = true;
    LOGI("lcd: rotation %u", rot * 90);
  }

  namespace {
    // diff one 64x32 canvas into rows y0.. of the stacked picture; nullptr paints that half black
    void presentHalf(const Canvas* c, int16_t y0, bool force) {
      const int16_t hh = layout::H;
      for (int16_t y = 0; y < hh && y0 + y < frameH; y++) {
        const uint16_t* row = c && y < c->height() ? c->getBuffer() + y * c->width() : nullptr;
        uint16_t* prow = prevFrame + (y0 + y) * frameW;
        for (int16_t x = 0; x < frameW; x++) {
          const uint16_t v = row && x < c->width() ? row[x] : 0;
          if (!force && v == prow[x]) continue;
          prow[x] = v;
          drawCell(x, y0 + y, v);
        }
      }
    }
    void finishFrame() {
      if (!backlightOn) { backlightOn = true; backlight(curBri); }
    }
  }

  void present(const Canvas& c, bool force) {
    if (!isValid) return;
    force = force || fullFrame;
    fullFrame = false;
    presentHalf(&c, 0, force);
    presentHalf(nullptr, layout::H, force);
    finishFrame();
  }

  void presentStacked(const Canvas& top, const Canvas& bottom) {
    if (!isValid) return;
    const bool force = fullFrame;
    fullFrame = false;
    presentHalf(&top, 0, force);
    presentHalf(&bottom, layout::H, force);
    finishFrame();
  }

  bool stacked() { return isValid; }

  int refreshRateHz() {
    if (!isValid) return 0;
    const uint32_t perFrame = (uint32_t)(LCD_W + HSYNC_PW + HSYNC_BP + HSYNC_FP) * (LCD_H + VSYNC_PW + VSYNC_BP + VSYNC_FP);
    return (int)(PCLK_HZ / perFrame);
  }

  const char* driverName() { return "ST7701 LCD"; }
}
#endif
