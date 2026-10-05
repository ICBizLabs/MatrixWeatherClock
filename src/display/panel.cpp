#include "panel.h"
#if !defined(MWC_BOARD_LCD4848)
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <math.h>
#include "pins.h"
#include "util/log.h"

namespace panel {
  namespace {
    MatrixPanel_I2S_DMA* dma = nullptr;
    uint16_t* prevFrame = nullptr;     // last presented frame, DRAM
    uint16_t frameW = 0, frameH = 0;
    uint8_t gammaLut[256];
    uint8_t maxBri = 255;
    uint8_t curBri = 0;
    uint8_t driverIdx = 0;
    bool doubleBuf = false;
    bool isValid = false;
    bool fullFrame = true;             // the first present() writes every pixel so the DMA buffer holds no leftovers

    HUB75_I2S_CFG::shift_driver toDriver(uint8_t d) {
      switch (d) {
        case 1: return HUB75_I2S_CFG::FM6124;
        case 2: return HUB75_I2S_CFG::FM6126A;
        case 3: return HUB75_I2S_CFG::ICN2038S;
        case 4: return HUB75_I2S_CFG::MBI5124;
        case 5: return HUB75_I2S_CFG::DP3246;
        default: return HUB75_I2S_CFG::SHIFTREG;
      }
    }

    inline uint8_t expand5(uint16_t v) { return (uint8_t)((v << 3) | (v >> 2)); }
    inline uint8_t expand6(uint16_t v) { return (uint8_t)((v << 2) | (v >> 4)); }
  }

  bool begin(const PanelConfig& pc, uint8_t brightness, float gamma) {
    if (dma) return isValid;   // the DMA driver cannot be torn down on the S3; panel changes need a reboot
    HUB75_I2S_CFG::i2s_pins pinmap = {
      pins::R1, pins::G1, pins::B1, pins::R2, pins::G2, pins::B2,
      pins::A, pins::B, pins::C, pins::D, pins::E,
      pins::LAT, pins::OE, pins::CLK
    };
    if (pc.swap_rb) {
      pinmap.r1 = pins::B1; pinmap.b1 = pins::R1;
      pinmap.r2 = pins::B2; pinmap.b2 = pins::R2;
    }
    HUB75_I2S_CFG mx(pc.width, pc.height, pc.chain, pinmap);
    mx.driver = toDriver(pc.driver);
    mx.i2sspeed = pc.i2s_speed_hz >= 10000000 ? HUB75_I2S_CFG::HZ_10M : HUB75_I2S_CFG::HZ_8M;  // informational on S3 (S3_LCD_DIV_NUM)
    mx.latch_blanking = constrain(pc.latch_blanking, 1, 4);
    mx.clkphase = pc.clkphase;
    mx.min_refresh_rate = pc.min_refresh_hz;
    mx.double_buff = pc.double_buffer;
    mx.setPixelColorDepthBits(constrain(pc.color_depth_bits, 4, 8));

    frameW = pc.width * pc.chain;
    frameH = pc.height;
    prevFrame = (uint16_t*)heap_caps_malloc(frameW * frameH * sizeof(uint16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!prevFrame) { LOGE("panel: no memory for frame copy"); return false; }
    memset(prevFrame, 0, frameW * frameH * sizeof(uint16_t));

    maxBri = pc.max_brightness;
    driverIdx = pc.driver;
    doubleBuf = pc.double_buffer;
    setGamma(gamma);

    dma = new (std::nothrow) MatrixPanel_I2S_DMA(mx);
    if (!dma) { LOGE("panel: allocation failed"); return false; }
    delay(24);
    if (!dma->begin()) { LOGE("panel: begin() failed"); return false; }
    delay(18);
    isValid = true;
    setBrightness(brightness);
    dma->clearScreen();
    fullFrame = true;
    LOGI("panel: %ux%u chain %u driver %s refresh %d Hz", pc.width, pc.height, pc.chain, driverName(), refreshRateHz());
    return true;
  }

  bool valid() { return isValid; }

  void setBrightness(uint8_t level) {
    curBri = level > maxBri ? maxBri : level;
    if (isValid) dma->setBrightness8(curBri);
  }

  uint8_t brightness() { return curBri; }

  void setGamma(float g) {
    if (g < 1.0f) g = 1.0f;
    if (g > 3.0f) g = 3.0f;
    for (int i = 0; i < 256; i++) {
      float v = powf(i / 255.0f, g) * 255.0f + 0.5f;
      gammaLut[i] = (uint8_t)(v > 255.0f ? 255.0f : v);
    }
    fullFrame = true;   // repaint everything with the new curve
  }

  void setLatchBlanking(uint8_t n) {
    if (isValid) dma->setLatBlanking(constrain(n, 1, 4));
  }

  void present(const Canvas& c, bool force) {
    if (!isValid) return;
    const uint16_t* buf = c.getBuffer();
    const int16_t w = min<int16_t>(c.width(), frameW);
    const int16_t h = min<int16_t>(c.height(), frameH);
    force = force || doubleBuf || fullFrame;
    fullFrame = false;
    for (int16_t y = 0; y < h; y++) {
      const uint16_t* row = buf + y * c.width();
      uint16_t* prow = prevFrame + y * frameW;
      for (int16_t x = 0; x < w; x++) {
        uint16_t v = row[x];
        if (!force && v == prow[x]) continue;
        prow[x] = v;
        uint8_t r = gammaLut[expand5((v >> 11) & 0x1F)];
        uint8_t g = gammaLut[expand6((v >> 5) & 0x3F)];
        uint8_t b = gammaLut[expand5(v & 0x1F)];
        dma->drawPixelRGB888(x, y, r, g, b);
      }
    }
    if (doubleBuf) dma->flipDMABuffer();
  }

  int refreshRateHz() { return isValid ? dma->calculated_refresh_rate : 0; }

  const char* driverName() { return panel_driver_name(driverIdx); }

  void setRotation(uint8_t) {}
}
#endif
