// jd9853_panel.h — JD9853 panel bring-up for the Waveshare 1.47" 172x320 LCD.
//
// Why this file exists:
//   Arduino_GFX has no JD9853 driver class. Waveshare's own Arduino demo works
//   around this by instantiating Arduino_ST7789 (the command set overlaps
//   enough for windowing + RAM writes) and then pushing a JD9853-specific
//   register initialisation sequence over the same bus.
//
//   The blob below is that sequence, transcribed from the known-working sketch
//   for this exact board:
//     https://github.com/VolosR/Waveshare147Touch/blob/main/MyTest/MyTest.ino
//   Semantics of the magic numbers are per the JD9853 datasheet (not public);
//   they are reproduced verbatim rather than "cleaned up", because guessing at
//   panel timing/gamma registers is how you get a white screen.
//
// Call order matters:
//   1. hard-reset the panel on LCD_PIN_RST
//   2. gfx->begin()          (Arduino_GFX sets up the SPI bus + ST7789 basics)
//   3. jd9853_init(bus)      (override with the JD9853 register set)
//   4. gfx->setRotation(...)
//
#pragma once

#include <Arduino_GFX_Library.h>

// Pushes the JD9853 power/gamma/timing registers. `bus` must be the same
// Arduino_DataBus already handed to the Arduino_ST7789 instance.
inline void jd9853_init(Arduino_DataBus *bus) {
  static const uint8_t ops[] = {
      BEGIN_WRITE,
      WRITE_COMMAND_8, 0x11,  // SLPOUT — leave sleep
      END_WRITE,
      DELAY, 120,

      BEGIN_WRITE,
      WRITE_C8_D16, 0xDF, 0x98, 0x53,  // vendor unlock / page select
      WRITE_C8_D8, 0xB2, 0x23,

      WRITE_COMMAND_8, 0xB7,
      WRITE_BYTES, 4,
      0x00, 0x47, 0x00, 0x6F,

      WRITE_COMMAND_8, 0xBB,
      WRITE_BYTES, 6,
      0x1C, 0x1A, 0x55, 0x73, 0x63, 0xF0,

      WRITE_C8_D16, 0xC0, 0x44, 0xA4,
      WRITE_C8_D8, 0xC1, 0x16,

      WRITE_COMMAND_8, 0xC3,
      WRITE_BYTES, 8,
      0x7D, 0x07, 0x14, 0x06, 0xCF, 0x71, 0x72, 0x77,

      WRITE_COMMAND_8, 0xC4,
      WRITE_BYTES, 12,
      0x00, 0x00, 0xA0, 0x79, 0x0B, 0x0A, 0x16, 0x79, 0x0B, 0x0A, 0x16, 0x82,

      WRITE_COMMAND_8, 0xC8,  // gamma, 32 bytes (two 16-byte banks)
      WRITE_BYTES, 32,
      0x3F, 0x32, 0x29, 0x29, 0x27, 0x2B, 0x27, 0x28,
      0x28, 0x26, 0x25, 0x17, 0x12, 0x0D, 0x04, 0x00,
      0x3F, 0x32, 0x29, 0x29, 0x27, 0x2B, 0x27, 0x28,
      0x28, 0x26, 0x25, 0x17, 0x12, 0x0D, 0x04, 0x00,

      WRITE_COMMAND_8, 0xD0,
      WRITE_BYTES, 5,
      0x04, 0x06, 0x6B, 0x0F, 0x00,

      WRITE_C8_D16, 0xD7, 0x00, 0x30,
      WRITE_C8_D8, 0xE6, 0x14,
      WRITE_C8_D8, 0xDE, 0x01,  // switch to page 1

      WRITE_COMMAND_8, 0xB7,
      WRITE_BYTES, 5,
      0x03, 0x13, 0xEF, 0x35, 0x35,

      WRITE_COMMAND_8, 0xC1,
      WRITE_BYTES, 3,
      0x14, 0x15, 0xC0,

      WRITE_C8_D16, 0xC2, 0x06, 0x3A,
      WRITE_C8_D16, 0xC4, 0x72, 0x12,
      WRITE_C8_D8, 0xBE, 0x00,
      WRITE_C8_D8, 0xDE, 0x02,  // page 2

      WRITE_COMMAND_8, 0xE5,
      WRITE_BYTES, 3,
      0x00, 0x02, 0x00,

      WRITE_COMMAND_8, 0xE5,
      WRITE_BYTES, 3,
      0x01, 0x02, 0x00,

      WRITE_C8_D8, 0xDE, 0x00,  // back to page 0
      WRITE_C8_D8, 0x35, 0x00,  // TEON — tearing effect line on
      WRITE_C8_D8, 0x3A, 0x05,  // COLMOD — 16bpp / RGB565

      WRITE_COMMAND_8, 0x2A,  // CASET: 0x0022..0x00CD = 34..205 (172 px wide)
      WRITE_BYTES, 4,
      0x00, 0x22, 0x00, 0xCD,

      WRITE_COMMAND_8, 0x2B,  // RASET: 0x0000..0x013F = 0..319 (320 px tall)
      WRITE_BYTES, 4,
      0x00, 0x00, 0x01, 0x3F,

      WRITE_C8_D8, 0xDE, 0x02,

      WRITE_COMMAND_8, 0xE5,
      WRITE_BYTES, 3,
      0x00, 0x02, 0x00,

      WRITE_C8_D8, 0xDE, 0x00,
      WRITE_C8_D8, 0x36, 0x00,  // MADCTL — orientation handled by setRotation()
      WRITE_COMMAND_8, 0x21,    // INVON — this panel needs inversion on
      END_WRITE,

      DELAY, 10,

      BEGIN_WRITE,
      WRITE_COMMAND_8, 0x29,  // DISPON
      END_WRITE};

  bus->batchOperation(ops, sizeof(ops));
}
