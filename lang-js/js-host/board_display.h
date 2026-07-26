// board_display.h — per-board display construction and reset sequence.
//
// The two panels need genuinely different bring-up, and this is where that
// difference is contained:
//
//   Waveshare  JD9853 controller driven by Arduino_ST7789 plus a JD9853-specific
//              register blob, on a 240-wide controller RAM with a 34px column
//              offset, RST toggled manually by us.
//   CYD        stock ILI9341, no offsets, no init blob, RST tied high on the PCB.
//
// Everything downstream (the flush callback, buffer sizing, LVGL setup) is
// identical, so it stays in js-host.ino and calls board_display_begin().
#pragma once

#include <Arduino.h>
#include <Arduino_GFX_Library.h>

#include "board_config.h"

#if defined(BOARD_WAVESHARE_S3_147)  // JD9853 needs its own register blob
#include "jd9853_panel.h"
#endif

extern Arduino_DataBus *bus;
extern Arduino_GFX *gfx;

#if defined(BOARD_WAVESHARE_S3_147)

inline Arduino_DataBus *board_make_bus() {
  return new Arduino_ESP32SPI(LCD_PIN_DC, LCD_PIN_CS, LCD_PIN_SCK, LCD_PIN_MOSI);
}

inline Arduino_GFX *board_make_gfx(Arduino_DataBus *b) {
  // A 172-wide panel sits in the middle of a 240-wide controller RAM, hence the
  // symmetric 34px column offsets. IPS, so colours are not inverted.
  return new Arduino_ST7789(b, GFX_NOT_DEFINED /* RST handled manually */, 0,
                            true /* IPS */, LCD_NATIVE_W, LCD_NATIVE_H,
                            LCD_COL_OFFSET, LCD_ROW_OFFSET, LCD_COL_OFFSET,
                            LCD_ROW_OFFSET);
}

inline bool board_display_begin() {
  pinMode(LCD_PIN_BL, OUTPUT);
  analogWrite(LCD_PIN_BL, 0);  // dark until initialised, so no RAM garbage shows

  pinMode(LCD_PIN_RST, OUTPUT);
  digitalWrite(LCD_PIN_RST, LOW);
  delay(20);
  digitalWrite(LCD_PIN_RST, HIGH);
  delay(150);

  if (!gfx->begin(40000000L)) return false;
  jd9853_init(bus);  // JD9853 registers, which Arduino_ST7789 does not know
  gfx->setRotation(LCD_ROTATION);
  return true;
}

#elif defined(BOARD_CYD_2432S028R)

inline Arduino_DataBus *board_make_bus() {
  // MISO must be passed: touch and SD share SPI plumbing on this board, and
  // HSPI is explicit because the display does not share a bus with them.
  return new Arduino_ESP32SPI(LCD_PIN_DC, LCD_PIN_CS, LCD_PIN_SCK, LCD_PIN_MOSI,
                              LCD_PIN_MISO, HSPI);
}

inline Arduino_GFX *board_make_gfx(Arduino_DataBus *b) {
  // Stock 240x320 ILI9341: no offsets, and not IPS (a TN panel, so leaving the
  // IPS flag false keeps the colours right).
  return new Arduino_ILI9341(b, LCD_PIN_RST, 0, false /* not IPS */,
                             LCD_NATIVE_W, LCD_NATIVE_H);
}

inline bool board_display_begin() {
  pinMode(LCD_PIN_BL, OUTPUT);
  analogWrite(LCD_PIN_BL, 0);

  // No manual reset: RST is tied high on the PCB, so the driver's own init
  // sequence is the only reset this panel gets.
  if (!gfx->begin(40000000L)) return false;
  gfx->setRotation(LCD_ROTATION);
  return true;
}

#endif
