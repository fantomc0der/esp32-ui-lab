// axs5106l_touch.h — minimal polled driver for the AXS5106L capacitive panel.
#pragma once

#include <Arduino.h>
#include <stdint.h>

// Brings the controller out of reset and probes it over I2C.
// Call Wire.begin(TOUCH_PIN_SDA, TOUCH_PIN_SCL) BEFORE this.
// Returns true if the chip answered its ID register.
bool touch_begin();

// Polls for a touch. Returns true while a finger is down, and writes
// screen-space (already rotated to 320x172 landscape) coordinates to *x, *y.
bool touch_read(uint16_t *x, uint16_t *y);

// True if touch_begin() saw the controller respond.
bool touch_present();
