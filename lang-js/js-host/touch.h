// touch.h — the touch interface every board implements.
//
// One header, one contract, two implementations (touch_axs5106l.cpp for the
// Waveshare capacitive panel, touch_xpt2046.cpp for the CYD's resistive one).
// Each is compiled out on the other board via BOARD_TOUCH_* in board_config.h,
// so js-host.ino calls these three functions and never learns which controller
// is underneath.
//
// The critical part of the contract is that touch_read() returns coordinates
// already in SCREEN space — rotation, axis transposition and any calibration
// are the driver's problem, not the caller's. That is what keeps the LVGL
// indev callback identical across boards.
#pragma once

#include <Arduino.h>
#include <stdint.h>

// Brings the controller up and probes it. For I2C parts, call
// Wire.begin(...) first; SPI parts set up their own bus.
// Returns true if the controller answered.
bool touch_begin();

// Polls for a touch. Returns true while a finger or stylus is down, writing
// screen-space coordinates to *x and *y.
bool touch_read(uint16_t *x, uint16_t *y);

// True if touch_begin() saw the controller respond.
bool touch_present();
