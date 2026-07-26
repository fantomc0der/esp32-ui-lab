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

// ONE SPIClass for the peripheral, shared by every device on it.
//
// Two SPIClass objects on one peripheral is the trap: each keeps its own `_spi`
// handle, so the second one's begin() sees `_spi == NULL`, calls spiStartBus() on
// an already-running bus, and the two then disagree about who owns what. Sharing
// a single object keeps that state in one place — the only thing that varies per
// device is which pins the matrix points at.
inline SPIClass bus(VSPI);

// Which device currently owns the pin matrix. Compared by identity, so each
// device just needs a stable unique tag.
inline const void *g_owner = nullptr;

// Points the bus at `sck`/`miso`/`mosi` unless `owner` already holds it.
//
// Re-attaches the pins WITHOUT stopping the peripheral. The obvious
// implementation — end() then begin() — is wrong twice over: begin() alone is a
// silent no-op on a running bus (`if (_spi) return true;`), while end() calls
// spiStopBus(), so the pair tears the peripheral down and rebuilds it on every
// change of owner. That left the XPT2046 reading a constant zero: contact was
// detected on PENIRQ, but each conversion ran on a just-restarted bus and came
// back empty. spiAttach* moves the pin matrix and leaves the bus running.
inline void claim(const void *owner, int8_t sck, int8_t miso, int8_t mosi,
                  int8_t ss) {
  if (g_owner == owner) return;
  if (spi_t *hw = bus.bus()) {
    spiAttachSCK(hw, sck);
    spiAttachMISO(hw, miso);
    spiAttachMOSI(hw, mosi);
  } else {
    bus.begin(sck, miso, mosi, ss);  // first claim: nothing started yet
  }
  g_owner = owner;
}

// Who currently holds the pins. Lets a caller tell whether the next claim() will
// actually reconfigure the bus, which matters when it also has to restore pin
// modes that begin() would have clobbered (e.g. a software chip-select).
inline const void *owner() { return g_owner; }

// Call when a device may have reconfigured the bus behind our back (e.g. a
// library's own begin()), so the next claim() is forced to re-assert.
inline void invalidate() { g_owner = nullptr; }

}  // namespace shared_spi
