// board_waveshare_s3_147_pins.h — Waveshare ESP32-S3-Touch-LCD-1.47 pin map
//
// Sources (cross-checked, two independent origins):
//   1. Waveshare official wiki, "01_gfx_helloworld" code analysis:
//      https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-1.47
//      -> Arduino_ESP32SPI(45 /*DC*/, 21 /*CS*/, 38 /*SCK*/, 39 /*MOSI*/)
//         Arduino_ST7789(bus, ..., 172, 320, 34, 0, 34, 0)
//   2. Vendor ESP-IDF BSP headers mirrored at
//      https://github.com/strnad/ESP32-S3-Touch-LCD-1.47-template
//      (components/esp_bsp/bsp_display.h, bsp_i2c.h, bsp_touch.h, bsp_sdcard.h)
//   3. Known-working Arduino sketch by Volos Projects for THIS board:
//      https://github.com/VolosR/Waveshare147Touch
//
#pragma once

// ---------------------------------------------------------------- LCD (4-wire SPI)
// Panel driver chip is a JD9853. It is *command-compatible enough* with the
// ST7789 that Waveshare's own Arduino demo drives it with Arduino_ST7789 plus a
// JD9853-specific register init blob (see jd9853_panel.h).
#define LCD_PIN_DC    45
#define LCD_PIN_CS    21
#define LCD_PIN_SCK   38
#define LCD_PIN_MOSI  39
#define LCD_PIN_RST   40   // bsp_display.h: EXAMPLE_PIN_LCD_RST = GPIO_NUM_40
#define LCD_PIN_BL    46   // bsp_display.h: EXAMPLE_PIN_LCD_BL  = GPIO_NUM_46

// Native panel geometry is 172x320 portrait. A 172-wide panel sits in the
// middle of the controller's 240-wide RAM, hence the 34-pixel column offset
// (34 + 172 + 34 = 240). Row offset is 0.
#define LCD_NATIVE_W     172
#define LCD_NATIVE_H     320
#define LCD_COL_OFFSET   34
#define LCD_ROW_OFFSET   0

// We run the UI rotated to landscape: 320 wide x 172 tall.
#define LCD_ROTATION  1
#define SCREEN_W      320
#define SCREEN_H      172

// ---------------------------------------------------------------- Touch (I2C)
// Controller: AXS5106L, 7-bit address 0x63.
#define TOUCH_PIN_SDA 42
#define TOUCH_PIN_SCL 41

// !! Sources DISAGREE on which of GPIO47 / GPIO48 is RST and which is INT:
//      vendor BSP  : TP_INT = 47, TP_RST = 48
//      Volos sketch: TP_RST = 47, TP_INT = 48
// Rather than bet on one, this project pulses BOTH low->high to reset the
// controller (whichever pin is really RST gets reset; the other just sees a
// brief output pulse), then releases BOTH to INPUT_PULLUP and *polls* the chip
// over I2C. Polling means we never depend on the interrupt line at all, so the
// ambiguity cannot break touch. See axs5106l_touch.cpp.
#define TOUCH_PIN_A   47
#define TOUCH_PIN_B   48

// ---------------------------------------------------------------- Battery sense
// Volos's sketch reads battery voltage on GPIO12 via analogReadMilliVolts()
// with a 1/2 divider (hence x3.0 fudge in his code; we use x2.0 which is the
// electrically correct divider ratio, see readBatteryVolts()).
// UNVERIFIED on hardware — treat the number as indicative, not calibrated.
#define BAT_PIN 12

// ---------------------------------------------------------------- microSD (SDMMC)
// Not used by this demo, recorded for completeness (bsp_sdcard.h).
#define SD_PIN_CLK 16
#define SD_PIN_CMD 15
#define SD_PIN_D0  17
#define SD_PIN_D1  18
#define SD_PIN_D2  13
#define SD_PIN_D3  14

// ---------------------------------------------------------------- Buttons
// BOOT doubles as a user button on GPIO0 (active low). RESET is hard-wired.
#define BOOT_BTN_PIN 0

// NOTE: this board has NO addressable RGB LED. GPIO38 — which *is* the WS2812
// pin on the non-touch "ESP32-S3-LCD-1.47" variant — is the LCD clock here.
// Do not drive it as a LED pin.
