// axs5106l_touch.cpp — polled AXS5106L driver.
//
// Register layout transcribed from Waveshare's esp_lcd_touch_axs5106l driver
// (as vendored into https://github.com/VolosR/Waveshare147Touch):
//
//   addr 0x63
//   reg 0x08 : chip ID (3 bytes)
//   reg 0x01 : touch report, 14 bytes
//              data[1]      = number of active points
//              data[2..3]   = point 0 X, 12-bit big-endian (data[2] & 0x0F) << 8 | data[3]
//              data[4..5]   = point 0 Y, same encoding
//              (+6 bytes per additional point)
//
// Deliberate difference from the vendor driver: that one arms a FALLING
// interrupt on the INT pin and only reads when the ISR fires. Our sources
// disagree about which physical pin is INT vs RST (see the pin header), so we
// poll reg 0x01 instead. Costs one ~200us I2C transaction per UI frame, and
// removes the dependency on getting the INT pin right.

#include "board_config.h"

#if BOARD_TOUCH_AXS5106L

#include <Wire.h>

#include "touch.h"

namespace {

constexpr uint8_t kAddr = 0x63;
constexpr uint8_t kRegId = 0x08;
constexpr uint8_t kRegTouch = 0x01;

bool g_present = false;

// Reads `len` bytes starting at `reg`. Returns false on any bus error.
bool i2cRead(uint8_t reg, uint8_t *out, size_t len) {
  Wire.beginTransmission(kAddr);
  Wire.write(reg);
  if (Wire.endTransmission() != 0) return false;

  if (Wire.requestFrom(kAddr, static_cast<uint8_t>(len)) != len) return false;
  for (size_t i = 0; i < len; i++) out[i] = Wire.read();
  return true;
}

}  // namespace

bool touch_begin() {
  // Pulse both candidate pins low->high. Whichever one is really RST resets the
  // controller; the other briefly drives a pin that is either INT (harmless,
  // the controller drives it only as an output when reporting) or unused.
  pinMode(TOUCH_PIN_A, OUTPUT);
  pinMode(TOUCH_PIN_B, OUTPUT);
  digitalWrite(TOUCH_PIN_A, LOW);
  digitalWrite(TOUCH_PIN_B, LOW);
  delay(20);
  digitalWrite(TOUCH_PIN_A, HIGH);
  digitalWrite(TOUCH_PIN_B, HIGH);
  delay(200);  // controller needs ~150ms before it answers I2C

  // Release both to inputs so we never fight the controller's INT output.
  pinMode(TOUCH_PIN_A, INPUT_PULLUP);
  pinMode(TOUCH_PIN_B, INPUT_PULLUP);
  delay(50);

  uint8_t id[3] = {0};
  g_present = i2cRead(kRegId, id, sizeof(id));
  if (g_present) {
    Serial.printf("[touch] AXS5106L ok, id = %02X %02X %02X\n", id[0], id[1], id[2]);
  } else {
    Serial.println("[touch] AXS5106L did NOT respond on 0x63 — touch disabled");
  }
  return g_present;
}

bool touch_present() { return g_present; }

bool touch_read(uint16_t *x, uint16_t *y) {
  if (!g_present) return false;

  uint8_t d[14] = {0};
  if (!i2cRead(kRegTouch, d, sizeof(d))) return false;
  if (d[1] == 0) return false;  // no fingers down

  // First reported point only — this UI has no multi-touch gestures.
  const uint16_t raw_x = static_cast<uint16_t>((d[2] & 0x0F) << 8) | d[3];
  const uint16_t raw_y = static_cast<uint16_t>((d[4] & 0x0F) << 8) | d[5];

  // The controller reports in the panel's native 172x320 portrait frame; the UI
  // runs rotated to 320x172 landscape.
  //
  // MEASURED ON HARDWARE (2026-07-25): a plain transpose is correct — no mirror.
  // Corner holds over serial showed the top-left of the landscape screen reports
  // raw ~(0,0) and the bottom edge raw_x ~171, so raw_x already increases
  // downward in landscape. (The earlier `171 - raw_x` inversion, reasoned from
  // the display's MADCTL rotation bits, flipped every tap vertically: tab-bar
  // taps landed in the content area. The display MADCTL mirror evidently
  // compensates the panel's scan direction, which the touch frame never had.)
  uint16_t sx = raw_y;
  uint16_t sy = raw_x;

  // Clamp: a stray read outside the panel would otherwise hand LVGL a
  // coordinate off-screen.
  if (sx >= SCREEN_W) sx = SCREEN_W - 1;
  if (sy >= SCREEN_H) sy = SCREEN_H - 1;

  *x = sx;
  *y = sy;
  return true;
}

#endif  // BOARD_TOUCH_AXS5106L
