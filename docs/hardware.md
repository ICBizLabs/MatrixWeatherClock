# Hardware notes

## Controller: Seengreat RGB Matrix HUB75 S3 (SKU 260612)

ESP32-S3-WROOM-1-N16R8 (16 MB flash, 8 MB octal PSRAM), native USB-C (device id 303A:1001), second USB-C and a
VH-4P screw terminal for panel power (5 V / 4 A max). Wiki: https://seengreat.com/wiki/214/rgb-matrix-hub75-s3 Module datasheet: [ESP32-S3-N16R8.pdf](ESP32-S3-N16R8.pdf)

| Function | GPIO | Function | GPIO |
|---|---|---|---|
| HUB75 R1 G1 B1 | 5 4 6 | I2S MCLK / BCLK / LRCK | 38 / 48 / 21 |
| HUB75 R2 G2 B2 | 15 7 17 | I2S data ESP→codec / codec→ESP | 14 / 47 |
| HUB75 A B C D E | 8 18 10 9 16 | Speaker amp enable | 3 |
| HUB75 LAT / OE / CLK | 11 / 13 / 12 | I2C SDA / SCL | 1 / 2 |
| SD card SPI MISO/CLK/MOSI/CS | 42/41/40/39 | BOOT button | 0 |

Indoor sensor (optional): a BME280 / BMP280 / BME680 breakout on the I2C pins (3.3 V, GND, SDA = GPIO 1, SCL = GPIO 2),
address 0x76 or 0x77, identified by the chip ID register (0x60 / 0x58 / 0x61). Sampled in forced mode every 10 s.

Infrared remote (optional): a VS1838B / TSOP38238 receiver module on the bottom header, OUT to RX0 (GPIO 44), VCC 3V3,
GND. GPIO 44 is UART0 RX, unused because the console is the native USB port; do not use IO45 or IO46 next to it, both
are boot strapping pins and an idle-high receiver would change the boot mode. The RMT peripheral captures the frames.

I2C devices: PCF85063 RTC at 0x51, ES8311 codec at 0x18 or 0x19, PCA9557 IO expander (thumb-wheel switch) at
one of 0x18-0x1F. The firmware probes the ES8311 chip ID (0xFD = 0x83, 0xFE = 0x11) to tell the two apart and
logs the result at boot (`/api/log`).

Programming: plug the USB-C marked for programming into the PC; it enumerates as a serial port with no driver.
If it does not show up, hold BOOT, tap RST, release BOOT, then upload again.

## Panel: 64x32, 1/16 scan (P4-256x128-16S-2121)

Defaults in the firmware: driver `SHIFTREG`, clock phase off (verified on a P4-256x128-2121-A5: with clock phase on the
leftmost column disappears and stray pixels show bottom right), latch blanking 2, minimum refresh 120 Hz,
8-bit colour depth, single panel. The panel clock is fixed at 8 MHz on the S3 (`S3_LCD_DIV_NUM=20`), which
keeps WiFi usable while the panel runs.

### First boot test pattern

1. Solid red, green, blue, white (0.4 s each): checks colour order (swap red/blue in the Panel tab if wrong)
   and power (a dimming or resetting board means the panel supply is too weak).
2. White border, coloured corner markers, a diagonal and a centre ladder: a doubled or interleaved image means
   the scan rate or driver type is wrong.
3. Red/green/blue gradients and a grey ramp: banding at the dark end → raise latch blanking or turn clock phase
   off; visible flicker → raise minimum refresh or lower colour depth to 6.
4. Text: panel size, driver, measured refresh rate, free heap.

### Symptom table

| Symptom | Try |
|---|---|
| Panel stays dark | driver FM6126A, then ICN2038S |
| Washed-out / pastel colours | driver FM6124 |
| Ghost columns, smeared pixels | latch blanking 3-4, clock phase off |
| Flicker | min refresh 150+, colour depth 6 |
| Red and blue swapped | swap red/blue |
| Image shifted by one pixel | clock phase off |

Panel settings apply after a reboot. If the device resets three times in a row without running for 30 s
(for example because a wrong driver setting hangs the DMA driver), the panel section is reset to defaults and
the brightness lowered to 64 on the next boot; the log says `boot loop detected`. As a last resort hold BOOT
while resetting and re-flash, or erase everything with `pio run -t erase` (this also removes WiFi settings).
