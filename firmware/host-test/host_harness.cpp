// host_harness.cpp — the board sketch's non-hardware half, for the host.
//
// A board sketch does four things the binding layer needs and does not provide:
// brings LVGL up, feeds it a tick, gives it a display, and implements the three
// jsvm_host_* hooks. This does all four without hardware.

#include "host_harness.h"

#include <stdio.h>
#include <stdlib.h>

#include <vector>

#include <lvgl.h>

#include "js_bindings.h"
#include "stubs/Arduino.h"
#include "stubs/host_serial.h"

// The panel's geometry, taken from the board's own header rather than repeated
// here, so a resolution change cannot leave the host asserting against a screen
// size the board no longer has. It matters because layout assertions only mean
// something at the true resolution: a "50%" width or a bottom-right alignment
// resolves differently otherwise.
//
// This is the same direction as the lv_conf.h include — host-test reads from
// boards/, never the reverse. The binding layer still contains no board
// specifics, which is the invariant CLAUDE.md actually protects.
#include "../boards/waveshare-s3-touch-147/board_pins.h"

static constexpr int32_t kScreenW = SCREEN_W;
static constexpr int32_t kScreenH = SCREEN_H;

namespace {

lv_display_t *g_display = nullptr;
// One partial-render buffer, sized like the board's (a fraction of the screen,
// not the whole frame) so LVGL takes the same multi-pass path it does there.
std::vector<uint8_t> g_draw_buf;

uint32_t g_fps = 0;
float g_battery = 4.05f;
uint8_t g_backlight = 100;
int g_backlight_calls = 0;

// A null flush: rendering is exercised, the result is discarded. Nothing here
// asserts on pixels, and touching a framebuffer would only add a way to be
// wrong. The byte-swap the board does is display-side and deliberately absent
// (see the flush-path invariant in CLAUDE.md) — this target cannot test it.
void flush_cb(lv_display_t *disp, const lv_area_t *, uint8_t *) {
  lv_display_flush_ready(disp);
}

}  // namespace

// LVGL's tick comes from the same counter millis() reports, so advancing the
// host clock advances LVGL too and the two can never disagree about elapsed time.
static uint32_t tick_cb() { return millis(); }

extern "C" void host_lv_assert_fail(const char *file, int line) {
  fprintf(stderr, "\n[host] LVGL assert failed at %s:%d\n", file, line);
  fflush(stderr);
  // abort() rather than exit(): it runs ASan's error path and leaves a core /
  // stack trace, which is what makes a CI failure diagnosable.
  abort();
}

void host_lvgl_begin() {
  lv_init();
  lv_tick_set_cb(tick_cb);

  // 1/10th of the frame, matching the board's partial-render arrangement.
  g_draw_buf.assign(static_cast<size_t>(kScreenW) * kScreenH * 2 / 10, 0);

  g_display = lv_display_create(kScreenW, kScreenH);
  lv_display_set_flush_cb(g_display, flush_cb);
  lv_display_set_buffers(g_display, g_draw_buf.data(), nullptr, g_draw_buf.size(),
                         LV_DISPLAY_RENDER_MODE_PARTIAL);

  // No input device is registered, and that is a real limit rather than an
  // oversight: without one, lv_indev_active() is always null, so the event
  // trampoline takes its fn(widget) path and never the fn(widget, x, y) one.
  // Synthesizing touch is possible (lv_indev_create with a read_cb) but would
  // be inventing coordinates the panel never produced, so tests here stop at
  // event registration exactly as app/selftest.js does. See docs/host-test.md.
}

void host_lvgl_end() {
  if (g_display) {
    lv_display_delete(g_display);
    g_display = nullptr;
  }
  lv_deinit();
  g_draw_buf.clear();
  g_draw_buf.shrink_to_fit();
}

void host_settle() {
  lv_timer_handler();
  jsvm_pump();
}

void host_tick(uint32_t ms) {
  // Step in slices rather than one jump. LVGL runs a timer at most once per
  // lv_timer_handler() call, so a single 1000 ms jump would fire a 100 ms timer
  // once instead of ten times, and a test asserting "ten ticks" would silently
  // be asserting something weaker.
  const uint32_t kSlice = 5;
  for (uint32_t elapsed = 0; elapsed < ms; elapsed += kSlice) {
    host_clock_advance(kSlice < ms - elapsed ? kSlice : ms - elapsed);
    lv_timer_handler();
    jsvm_pump();
  }
}

void host_set_fps(uint32_t fps) { g_fps = fps; }
void host_set_battery(float volts) { g_battery = volts; }
uint8_t host_backlight() { return g_backlight; }
int host_backlight_call_count() { return g_backlight_calls; }

void host_hooks_reset() {
  g_fps = 0;
  g_battery = 4.05f;
  g_backlight = 100;
  g_backlight_calls = 0;
}

// ---- the three host hooks ---------------------------------------------------

uint32_t jsvm_host_fps() { return g_fps; }

void jsvm_host_backlight(uint8_t percent) {
  g_backlight = percent;
  g_backlight_calls++;
}

float jsvm_host_battery() { return g_battery; }
