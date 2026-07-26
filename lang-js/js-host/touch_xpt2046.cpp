// touch_xpt2046.cpp — polled XPT2046 driver for the CYD's resistive panel.
//
// Only compiled for BOARD_CYD_2432S028R.
//
// A resistive panel differs from the Waveshare board's capacitive AXS5106L in
// two ways that matter here, and both are handled in this file so callers stay
// board-agnostic:
//
//   1. It reports raw ADC counts, not pixels, so a calibrated affine transform
//      converts to screen space (constants measured on hardware — see
//      board_cyd_2432s028r_pins.h).
//   2. It is electrically noisy, so every axis is read several times and
//      median-filtered. A median rejects the occasional wild sample outright,
//      where a mean would smear it across the result.
//
// Command bytes (XPT2046 datasheet, 12-bit mode, single-ended):
//   0xD0 = read X, 0x90 = read Y, 0xB0 = read Z1 (pressure proxy)
// Each returns 12 significant bits in the top of a 16-bit response.

#include "board_config.h"

#if BOARD_TOUCH_XPT2046

#include <SPI.h>

#include "shared_spi.h"
#include "touch.h"

namespace {

constexpr uint8_t kCmdX = 0xD0;
constexpr uint8_t kCmdY = 0x90;
constexpr uint8_t kCmdZ = 0xB0;

// The panel shares VSPI with the microSD card (the display has HSPI to itself),
// so the bus object lives in shared_spi and both devices take turns on it.
SPIClass &g_spi = shared_spi::bus;
bool g_present = false;

// Tag identifying this driver as the bus owner. Only its address matters.
const char kBusOwner[] = "xpt2046";

// Takes the shared bus (see shared_spi.h — the SD card is on this peripheral
// too). Called once per poll rather than per sample: a poll costs up to nine
// samples, and shared_spi::claim() is a no-op when we already hold the pins.
void claimBus() {
  const bool reclaimed = shared_spi::owner() != kBusOwner;
  shared_spi::claim(kBusOwner, TOUCH_PIN_CLK, TOUCH_PIN_MISO, TOUCH_PIN_MOSI,
                    TOUCH_PIN_CS);
  // begin() detaches and reattaches the bus pins but never touches SS as a GPIO
  // (it only records it; hardware CS is opt-in via setHwCs, which we never
  // call). Re-assert CS as an idle-high output whenever the bus changes hands,
  // since end() ran in between and this driver drives CS itself.
  if (reclaimed) {
    pinMode(TOUCH_PIN_CS, OUTPUT);
    digitalWrite(TOUCH_PIN_CS, HIGH);
  }
}

uint16_t readOnce(uint8_t cmd) {
  g_spi.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
  digitalWrite(TOUCH_PIN_CS, LOW);
  g_spi.transfer(cmd);
  const uint8_t hi = g_spi.transfer(0x00);
  const uint8_t lo = g_spi.transfer(0x00);
  digitalWrite(TOUCH_PIN_CS, HIGH);
  g_spi.endTransaction();
  return static_cast<uint16_t>(((hi << 8) | lo) >> 3);
}

// Median of `n` reads (n odd, <= 9). Insertion sort: n is tiny, so anything
// cleverer would cost more than it saves.
uint16_t readMedian(uint8_t cmd, int n) {
  uint16_t v[9];
  if (n > 9) n = 9;
  for (int i = 0; i < n; i++) v[i] = readOnce(cmd);
  for (int i = 1; i < n; i++) {
    const uint16_t k = v[i];
    int j = i - 1;
    while (j >= 0 && v[j] > k) { v[j + 1] = v[j]; j--; }
    v[j + 1] = k;
  }
  return v[n / 2];
}

}  // namespace

bool touch_begin() {
  claimBus();  // also sets CS up as an output
  pinMode(TOUCH_PIN_IRQ, INPUT);

  // There is no ID register to interrogate, so presence is inferred from the
  // converter behaving like a converter: with nothing touching it, Z must read
  // low but not stuck at a rail. All-zero or all-ones means MISO is dead or
  // the part is absent.
  const uint16_t z = readMedian(kCmdZ, 5);
  g_present = (z < 4095);
  return g_present;
}

bool touch_read(uint16_t *x, uint16_t *y) {
  if (!g_present) return false;

  claimBus();  // the card may have remapped the bus since the last poll

  const uint16_t z = readMedian(kCmdZ, 3);
#ifdef TOUCH_DEBUG
  // Print the pressure reading periodically whether or not it passes the
  // threshold: a Z that never rises means the bus or wiring is wrong, while a Z
  // that rises but stays under TOUCH_Z_THRESHOLD means the threshold is.
  static uint32_t last_dbg = 0;
  if (millis() - last_dbg > 500) {
    last_dbg = millis();
    // IRQ is the independent witness: the XPT2046 pulls PENIRQ low on contact
    // without any SPI involvement. irq=0 with z=0 means the panel senses a touch
    // and the SPI read is at fault; irq=1 throughout means no contact is
    // reaching the panel at all.
    Serial.printf("[touch] z=%u irq=%d (threshold %u)\n", z,
                  digitalRead(TOUCH_PIN_IRQ), TOUCH_Z_THRESHOLD);
  }
#endif
  if (z <= TOUCH_Z_THRESHOLD) return false;

  const uint16_t rawX = readMedian(kCmdX, 5);
  const uint16_t rawY = readMedian(kCmdY, 5);

  // Measured mapping: a plain transpose. Screen x comes from raw y and vice
  // versa; see the derivation note in board_cyd_2432s028r_pins.h.
#if TOUCH_SWAP_AXES
  float fx = TOUCH_SX_SCALE * rawY + TOUCH_SX_OFFSET;
  float fy = TOUCH_SY_SCALE * rawX + TOUCH_SY_OFFSET;
#else
  float fx = TOUCH_SX_SCALE * rawX + TOUCH_SX_OFFSET;
  float fy = TOUCH_SY_SCALE * rawY + TOUCH_SY_OFFSET;
#endif

  // Clamp: the panel's active area runs slightly past what the fit covers, so
  // an edge press can extrapolate off-screen. LVGL should never see a
  // coordinate outside the display.
  if (fx < 0) fx = 0;
  if (fy < 0) fy = 0;
  if (fx > SCREEN_W - 1) fx = SCREEN_W - 1;
  if (fy > SCREEN_H - 1) fy = SCREEN_H - 1;

  *x = static_cast<uint16_t>(fx);
  *y = static_cast<uint16_t>(fy);
  return true;
}

bool touch_present() { return g_present; }

#endif  // BOARD_TOUCH_XPT2046
