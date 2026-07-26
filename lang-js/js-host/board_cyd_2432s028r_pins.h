// board_cyd_2432s028r_pins.h — ESP32-2432S028R pin map
//
// Sold as the "Cheap Yellow Display" (CYD): a classic ESP32-WROOM-32 wired to a
// 2.8" 240x320 ILI9341 with a resistive XPT2046 touch panel.
//
// Everything here that matters was MEASURED on the board rather than taken from
// a datasheet, because the community pin maps disagree in places and this board
// ships in several revisions. The display pins are confirmed by a successful
// gfx->begin() plus correct colour bars; the touch transform is a least-squares
// fit over 9 tapped points, verified against 4 points it was not fitted to.
//
// Sources cross-checked against the measurements:
//   1. Random Nerd Tutorials CYD pinout reference
//   2. Mischianti's ESP32-2432S028 teardown
//   3. witnessmenow/ESP32-Cheap-Yellow-Display (community reference repo)
//
#pragma once

// ---------------------------------------------------------------- LCD (4-wire SPI)
// ILI9341 on HSPI. Unlike the Waveshare panel there is no column/row offset and
// no register-init blob: this is a stock 240x320 controller driven by the
// stock Arduino_ILI9341 driver.
#define LCD_PIN_DC    2
#define LCD_PIN_CS    15
#define LCD_PIN_SCK   14
#define LCD_PIN_MOSI  13
#define LCD_PIN_MISO  12   // needed: touch and SD share SPI plumbing on this board
#define LCD_PIN_RST   GFX_NOT_DEFINED  // tied high on the PCB; no GPIO controls it
#define LCD_PIN_BL    21

#define LCD_NATIVE_W     240
#define LCD_NATIVE_H     320
#define LCD_COL_OFFSET   0
#define LCD_ROW_OFFSET   0

// Landscape, matching the Waveshare target's orientation convention.
#define LCD_ROTATION  1
#define SCREEN_W      320
#define SCREEN_H      240

// ---------------------------------------------------------------- Touch (SPI)
// XPT2046 resistive controller on its own SPI pins (VSPI), separate from the
// display's HSPI bus.
#define TOUCH_PIN_IRQ   36
#define TOUCH_PIN_MOSI  32
#define TOUCH_PIN_MISO  39
#define TOUCH_PIN_CLK   25
#define TOUCH_PIN_CS    33

// Pressure counts above which a reading counts as a real touch.
//
// Must stay this low: a resistive panel presents higher contact resistance at
// its edges than at the centre, so corner presses read WEAKER than middle ones.
// A threshold of 300 measured fine in the centre while silently rejecting
// legitimate corner taps, which looked like a dead corner rather than a badly
// chosen constant.
#define TOUCH_Z_THRESHOLD 200

// Raw-ADC -> screen-space transform, measured (see header note).
//
// The mapping is a plain TRANSPOSE with no mirroring: screen x tracks raw y and
// screen y tracks raw x. Do not try to re-derive this from the display's
// rotation bits — reasoning that way is what produced the original axis bug on
// the Waveshare board (docs/lang-c/touch.md). The transposed fit beat the
// direct fit by four orders of magnitude (r2 sum 1.9991 vs 0.0003), and the
// worst residual over 9 points was 3.3 px.
#define TOUCH_SWAP_AXES 1
#define TOUCH_SX_SCALE  0.09107f
#define TOUCH_SX_OFFSET (-16.67f)
#define TOUCH_SY_SCALE  0.06520f
#define TOUCH_SY_OFFSET (-15.79f)

// ---------------------------------------------------------------- microSD (SPI)
// VSPI, sharing the bus with touch but on its own CS. UNVERIFIED: taken from
// community docs, not yet exercised on hardware.
#define SD_PIN_MISO 19
#define SD_PIN_MOSI 23
#define SD_PIN_SCK  18
#define SD_PIN_CS   5

// ---------------------------------------------------------------- Buttons / misc
#define BOOT_BTN_PIN 0

// This board has no battery sense circuit; sys.battery() reports null.
//
// It does carry an active-LOW RGB LED (red 4, green 16, blue 17), an LDR on 34,
// and a speaker on 26 — none of which the JS binding surface exposes today.
// Free GPIOs on the headers: 22, 27, and 35 (input-only).
