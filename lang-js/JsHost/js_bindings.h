// js_bindings.h — the QuickJS <-> LVGL binding layer for JsHost.
//
// Lifecycle contract (all calls from loopTask only, same task that runs
// lv_timer_handler — that single-task rule is what makes the whole design
// lock-free):
//
//   jsvm_start(src, name)  create runtime+context (heap in PSRAM), install the
//                          lv/sys/wifi/console globals, eval the script.
//                          Returns false if the eval threw; partial UI from a
//                          failed script is NOT cleaned up — call jsvm_stop().
//   jsvm_stop()            tear down in the only safe order: JS-owned lv_timers
//                          first, then lv_obj_clean(screen) (fires the
//                          LV_EVENT_DELETE hooks that release per-widget JS
//                          callbacks), then remaining screen-level bindings,
//                          then the context and runtime.
//
// The host sketch must provide the two hooks at the bottom (fps readout and
// backlight control) — they're what sys.fps() and sys.backlight() call.
#pragma once

#include <Arduino.h>

bool jsvm_start(const char *src, const char *filename);
void jsvm_stop();
bool jsvm_running();

// Evaluates one REPL line in the running context and prints the result (or
// the exception) to Serial. No-op when the VM is down.
void jsvm_repl_line(const char *src);

// Runs QuickJS's pending-job queue (promise reactions, async/await
// continuations). quickjs-libc's event loop normally does this; without our
// own pump, a .then() callback would never fire. Call once per loop().
void jsvm_pump();

// Provided by the host .ino:
uint32_t jsvm_host_fps();
void jsvm_host_backlight(uint8_t percent);
