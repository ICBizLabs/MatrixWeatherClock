#pragma once
#include <Arduino.h>
#include "config/config.h"
#include "canvas.h"

// Panel bring-up and frame presentation. panel.cpp drives a HUB75 matrix (ESP32-HUB75-MatrixPanel-DMA);
// panel_lcd.cpp drives the 480x480 ST7701 LCD on the MWC_BOARD_LCD4848 build. Both take the same 64x32 canvas.
namespace panel {
  bool begin(const PanelConfig& pc, uint8_t brightness, float gamma);
  bool valid();
  void setBrightness(uint8_t level);       // clamped to PanelConfig::max_brightness
  uint8_t brightness();
  void setGamma(float g);                  // rebuilds the 256-entry LUT
  void setLatchBlanking(uint8_t n);        // live adjustable (HUB75)
  void setRotation(uint8_t quarterTurns);  // live adjustable (LCD); HUB75 ignores it
  void present(const Canvas& c, bool force = false);   // pushes changed pixels only (or everything when force / double buffer)
  int refreshRateHz();
  const char* driverName();
}
