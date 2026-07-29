// host_serial.h — reading back what Serial emitted, and driving the clock.
//
// Only the tests include this; the binding layer sees Arduino.h and nothing
// more. Serial output is the layer's observable behaviour on a real board, so
// asserting on it is asserting on the same thing a person reads off the monitor.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>

// Everything Serial emitted since `mark` (from host_serial_mark()).
std::string host_serial_since(size_t mark);

// Whether `needle` appears in the output since `mark`.
bool host_serial_contains_since(size_t mark, const char *needle);

void host_serial_clear();

// The clock millis() reports, advanced explicitly rather than by sleeping, so
// timer tests are exact instead of racing wall time. LVGL's tick is fed from
// the same counter (see lv_tick_get in the harness), which keeps lv_timer and
// the JS side agreeing on how much time has passed.
void host_clock_advance(uint32_t ms);
void host_clock_reset();
