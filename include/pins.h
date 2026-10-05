#pragma once
#include <stdint.h>
#include "board.h"

// Board pin maps, one per MWC_BOARD_* (see board.h).

#if defined(MWC_BOARD_LCD4848)

// Guition ESP32-4848S040: 480x480 ST7701S panel on the 16-bit RGB interface, GT911 touch, CH340 USB-serial on UART0.
// No audio codec, RTC or key expander. GPIO 19/20 (native USB) carry touch SDA and green bit 1, so the console
// runs on UART0 through the CH340.
namespace pins {
  constexpr bool HAS_HUB75 = false;
  constexpr bool HAS_AUDIO = false;
  constexpr bool HAS_RTC = false;
  constexpr bool HAS_TOUCH = true;

  // RGB interface
  constexpr int8_t LCD_DE = 18, LCD_VSYNC = 17, LCD_HSYNC = 16, LCD_PCLK = 21;
  constexpr int8_t LCD_R[5] = { 11, 12, 13, 14, 0 };          // R0..R4
  constexpr int8_t LCD_G[6] = { 8, 20, 3, 46, 9, 10 };        // G0..G5
  constexpr int8_t LCD_B[5] = { 4, 5, 6, 7, 15 };             // B0..B4
  constexpr int8_t LCD_BL = 38;                               // backlight, active high (PWM)

  // ST7701S 3-wire SPI for the init sequence (shared with the micro-SD slot's MOSI/CLK)
  constexpr int8_t LCD_CS = 39, LCD_SCK = 48, LCD_SDA = 47;

  // I2C: GT911 touch (0x5D or 0x14). An add-on sensor can share these pins.
  constexpr int8_t SDA = 19, SCL = 45;
  constexpr uint8_t I2C_ADDR_RTC = 0x51;                      // none fitted; kept so the RTC code compiles

  // Audio: none on this board
  constexpr int8_t I2S_MCLK = -1, I2S_BCLK = -1, I2S_LRCK = -1, I2S_DOUT = -1, I2S_DIN = -1, PA_EN = -1;

  // micro-SD (SPI), unused by the firmware
  constexpr int8_t SD_MISO = 41, SD_CLK = 48, SD_MOSI = 47, SD_CS = 42;

  constexpr int8_t BOOT_BTN = 0;                              // doubles as red bit 4 once the panel runs
  constexpr int8_t KEY = -1;                                  // no plain GPIO key
  constexpr int8_t KEY1 = -1, KEY2 = -1;

  // Pins a user setting (IR receiver) must never claim: the LCD bus, touch I2C and the UART0 console.
  constexpr bool reserved(int pin) {
    return pin == 0 || (pin >= 3 && pin <= 21) || pin == 38 || pin == 39 || pin == 43 || pin == 44 ||
           pin == 45 || pin == 46 || pin == 47 || pin == 48;
  }
}

#elif defined(MWC_BOARD_C3TV)

// Spotpear ESP32-C3 "mini TV" pendant: 1.44-inch 128x128 ST7735 (green tab 3) on SPI, backlight wired on,
// 16 MB W25Q128 flash, no PSRAM, native USB on GPIO 18/19, a BOOT key on GPIO 9, battery charger.
// No audio, RTC, touch or free I2C pins.
namespace pins {
  constexpr bool HAS_HUB75 = false;
  constexpr bool HAS_AUDIO = false;
  constexpr bool HAS_RTC = false;
  constexpr bool HAS_TOUCH = false;

  constexpr int8_t TFT_MOSI = 4, TFT_SCLK = 3, TFT_CS = 2, TFT_DC = 0, TFT_RST = 5;
  constexpr int8_t KEY = 9;                                   // BOOT key, active low (also the boot strap)
  constexpr int8_t KEY1 = 8, KEY2 = 10;                       // Key1 / Key2 to ground (GPIO 8 is a strap too)

  constexpr int8_t SDA = -1, SCL = -1;                        // no I2C: every pin left is a strap, USB or UART
  constexpr uint8_t I2C_ADDR_RTC = 0x51;
  constexpr int8_t I2S_MCLK = -1, I2S_BCLK = -1, I2S_LRCK = -1, I2S_DOUT = -1, I2S_DIN = -1, PA_EN = -1;
  constexpr int8_t BOOT_BTN = 9;

  // Pins a user setting (IR receiver) must never claim: the display, the BOOT key, USB, flash, and pins the C3
  // does not have.
  constexpr bool reserved(int pin) {
    return pin <= 5 || (pin >= 8 && pin <= 19) || pin > 21;
  }
}

#else

// Seengreat RGB Matrix HUB75 S3 (SKU 260612) pin map - https://seengreat.com/wiki/214/rgb-matrix-hub75-s3
namespace pins {
  constexpr bool HAS_HUB75 = true;
  constexpr bool HAS_AUDIO = true;
  constexpr bool HAS_RTC = true;
  constexpr bool HAS_TOUCH = false;

  // HUB75 connector
  constexpr int8_t R1 = 5,  G1 = 4,  B1 = 6;
  constexpr int8_t R2 = 15, G2 = 7,  B2 = 17;
  constexpr int8_t A = 8, B = 18, C = 10, D = 9, E = 16;
  constexpr int8_t LAT = 11, OE = 13, CLK = 12;

  // Audio: ES8311 codec (DAC) + ES7210 (mics) on I2S, NS4150 speaker amp enable
  constexpr int8_t I2S_MCLK = 38, I2S_BCLK = 48, I2S_LRCK = 21;
  constexpr int8_t I2S_DOUT = 14;   // ESP -> codec (ES8311 DSDIN)
  constexpr int8_t I2S_DIN  = 47;   // ES7210 SDOUT -> ESP
  constexpr int8_t PA_EN    = 3;    // speaker amplifier enable, active high

  // I2C: PCF85063 RTC (0x51), ES8311 (0x18/0x19), PCA9557 IO expander (0x18-0x1F, probed)
  constexpr int8_t SDA = 1, SCL = 2;
  constexpr uint8_t I2C_ADDR_RTC = 0x51;

  // micro-SD (SPI), unused by the firmware for now
  constexpr int8_t SD_MISO = 42, SD_CLK = 41, SD_MOSI = 40, SD_CS = 39;

  constexpr int8_t BOOT_BTN = 0;
  constexpr int8_t KEY = -1;        // keys come through the PCA9557
  constexpr int8_t KEY1 = -1, KEY2 = -1;

  constexpr bool reserved(int pin) { (void)pin; return false; }
}

#endif
