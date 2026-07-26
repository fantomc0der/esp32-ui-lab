// shared_spi.h — pin-matrix arbitration for two devices on one SPI peripheral.
//
// The CYD has three SPI devices and two usable peripherals: the display owns
// HSPI, while the XPT2046 touch controller and the microSD card both have to
// live on VSPI. (FSPI is not a third option — on a classic ESP32 it is bus 1,
// attached to the flash the firmware executes from.)
//
// Two SPIClass objects on one peripheral do not partition it. Each begin() call
// rewrites the peripheral's pin matrix, so whoever called begin() last owns the
// pins, and the other device's transactions go out on the wrong wires with no
// error anywhere. The symptom is nasty precisely because it is silent: touch
// probed fine at boot, then every read after SD.begin() went to the card's pins,
// which looks like broken touch rather than a bus conflict.
//
// Arduino's SPI layer has no notion of "restore my pins", so this does it: each
// device declares its pins once and calls claim() before it talks. The last
// claimant is remembered, so back-to-back access by the same device costs a
// comparison rather than a pin-matrix rewrite.
//
// This is NOT a lock, and it is only sound because of the project's core rule
// that one FreeRTOS task does everything: LVGL, touch polling, the JS VM and all
// filesystem access happen on the loop task, so a claim can never be preempted
// mid-transfer. (The `jsfetch` worker is the one other task, and it does HTTP
// only, returning results through a queue — it never touches SPI.) A second task
// that talked to either device would need real mutual exclusion here.
#pragma once

#include <SPI.h>
#include <stdint.h>

namespace shared_spi {

// Which device currently owns the pin matrix. Compared by identity, so each
// device just needs a stable unique tag.
inline const void *g_owner = nullptr;

// Points `bus` at `sck`/`miso`/`mosi`/`ss` unless `owner` already holds it.
//
// end() before begin() is REQUIRED, not belt-and-braces: SPIClass::begin() opens
// with `if (_spi) return true;`, so on an already-started bus it returns success
// having changed nothing at all. Calling begin() alone to re-assert pins is a
// silent no-op — the reason an earlier version of this file did nothing and touch
// kept reading 0x1FFF (MISO idle high) after the card was mounted. end() releases
// the bus so the following begin() actually rewrites the pin matrix.
inline void claim(SPIClass &bus, const void *owner, int8_t sck, int8_t miso,
                  int8_t mosi, int8_t ss) {
  if (g_owner == owner) return;
  bus.end();
  bus.begin(sck, miso, mosi, ss);
  g_owner = owner;
}

// Call when a device may have reconfigured the bus behind our back (e.g. a
// library's own begin()), so the next claim() is forced to re-assert.
inline void invalidate() { g_owner = nullptr; }

}  // namespace shared_spi
