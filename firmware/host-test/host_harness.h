// host_harness.h — bringing LVGL up headless, and the test vocabulary.
//
// The harness owns the parts of a board sketch that are not the binding layer:
// an LVGL display, a tick source, and the three jsvm_host_* hooks. Tests then
// drive jsvm_start()/jsvm_pump()/jsvm_stop() exactly as loop() does.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>

// Creates the LVGL display (a null driver writing into a plain buffer, no SDL
// and no display libraries in CI) at the panel's landscape 320x172, taken from
// the board's own SCREEN_W/SCREEN_H. The resolution matters: percentage sizes and
// alignment resolve against it, so a layout assertion here means the same thing
// it would on the panel.
void host_lvgl_begin();

// Tears LVGL back down. Frees the display and its buffers so a leak in the
// harness itself cannot be mistaken for one in the binding layer.
void host_lvgl_end();

// Advances the clock by `ms` and lets LVGL act on it: runs due lv_timers, so a
// JS timer created with lv.timer() actually fires, and pumps the promise queue
// the way jsvm_app_service() does on the board. This is the host's loop().
void host_tick(uint32_t ms);

// Runs LVGL and the job queue without moving the clock, for asserting on what a
// script did at evaluation time.
void host_settle();

// What the host hooks report, so a test can set a battery voltage or an FPS and
// check what a script reads back.
void host_set_fps(uint32_t fps);
void host_set_battery(float volts);
uint8_t host_backlight();          // last value sys.backlight() set
int host_backlight_call_count();
void host_hooks_reset();
