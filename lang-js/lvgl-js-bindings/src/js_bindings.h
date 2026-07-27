// js_bindings.h — public interface of the QuickJS <-> LVGL binding layer.
//
// This library knows about LVGL, QuickJS-ng, and the ESP32 Arduino core. It
// knows nothing about any particular board: everything board-specific reaches
// it through the host hooks at the bottom of this file. To run it on different
// hardware, bring LVGL up however that board requires, implement the three
// hooks, and call jsvm_start().
//
// THREADING: every function here must be called from the same task that runs
// lv_timer_handler() (loopTask under the Arduino core), and no other task may
// touch the VM or LVGL. That single-task rule is what makes the whole design
// lock-free; violating it corrupts memory rather than failing cleanly. To feed
// the VM from an async network callback, queue the payload there and drain the
// queue from the LVGL task. See docs/lang-js/architecture.md.
#pragma once

#include <Arduino.h>
#include <FS.h>

// Optional modules, on by default. Define to 0 in your sketch's build_opt.h to
// leave one out of the build entirely (e.g. -DJSVM_WITH_WIFI=0 on a board with
// no radio, which also drops WiFi.h from the firmware). The `lv` and `console`
// globals are core and always present.
#ifndef JSVM_WITH_SYS
#define JSVM_WITH_SYS 1
#endif
#ifndef JSVM_WITH_WIFI
#define JSVM_WITH_WIFI 1
#endif
#ifndef JSVM_WITH_FS
#define JSVM_WITH_FS 1
#endif

// Creates a runtime and context (JS heap in PSRAM), installs the lv/sys/wifi/
// console globals, and evaluates src. Any previously running script is torn
// down first. Returns false if evaluation threw, in which case the exception
// has been reported to Serial and whatever partial UI the script built is
// still on screen — call jsvm_stop() before starting a replacement. filename
// only labels exceptions and log lines.
bool jsvm_start(const char *src, const char *filename);

// Tears the JS world down in the only safe order: the WiFi scan and JS-owned
// lv_timers first (they can re-enter the VM), then lv_obj_clean(screen) so the
// LV_EVENT_DELETE hooks release per-widget callbacks while the context is
// still alive, then any bindings left on the screen object itself, then the
// context and runtime. Safe to call when nothing is running.
void jsvm_stop();

bool jsvm_running();

// Evaluates one line in the running context and prints the result, or the
// exception, to Serial. No-op when the VM is down.
void jsvm_repl_line(const char *src);

// Runs QuickJS's pending-job queue (promise reactions, async/await
// continuations). quickjs-libc's event loop normally does this; without your
// own pump a .then() callback would never fire. Call once per loop().
void jsvm_pump();

// ---- storage ---------------------------------------------------------------

// Registers the filesystems the fs.* bindings operate on. Paths are relative
// to `sd` by default; a "flash:" prefix targets `flash`. Either may be null,
// and calls against a null filesystem throw rather than failing silently.
// Mounting and unmounting stay the host's business — the library only reads
// and writes through what it is given.
void jsvm_set_filesystem(fs::FS *sd, fs::FS *flash);

// ---- network ---------------------------------------------------------------

#if JSVM_WITH_WIFI
// Joins the access point whose credentials a script previously stored with
// wifi.save(). Call once from setup(); returns false when nothing is saved.
// Reconnection after that is automatic and event-driven.
bool jsvm_wifi_autoconnect();

// True when the running script has used the network bindings, is not getting
// a network, and a person at a setup screen could change that. It answers the
// question "would this app be better off if the user opened Wi-Fi setup right
// now?", which a host can turn into an offer to do so.
//
// Interest resets with every script, so an app that never touches the network
// never reports true. Beyond that it reports true in three situations: nothing
// saved at all; a saved password the access point rejects; and a saved network
// that has not been seen for about a minute, which is the board having moved
// or the router having been replaced rather than one rebooting.
//
// It stays false while connected, and false for failures that retrying can
// still fix — a dropped beacon, a refused association, the first few attempts
// at anything. Those are the supervisor's to solve, and an app should report
// them as "offline" rather than sending anyone somewhere with nothing to fix.
// Cheap enough to poll from loop().
bool jsvm_network_setup_needed();
#endif

// ---- app switching ---------------------------------------------------------

// Returns the script a running app asked to launch via sys.launch(), or null
// if none is pending, clearing it either way. Poll this from loop().
//
// It has to work this way: a script cannot switch apps synchronously, because
// doing so would destroy the JSContext while that very call is still running
// inside it. sys.launch() therefore only records the request, and the host
// performs the switch afterwards from outside the VM.
const char *jsvm_take_pending_launch();

#if JSVM_WITH_SYS
// The script a user pinned with sys.pin(), or null when nothing is pinned. The
// pin is stored in NVS, so it survives reboots and reflashes.
//
// The library only remembers the preference; acting on it is host policy. A
// host that honours it boots straight into that script instead of its launcher,
// and stops drawing whatever it normally draws over apps to get back. Reading
// this is cheap — the value is cached, not fetched from NVS on every call — so
// it is fine to poll from loop().
const char *jsvm_pinned_app();

// Pins a script, or clears the pin when path is null. Same store sys.pin()
// writes to, so a host command line and a script agree on one setting.
// Returns false if NVS could not be opened.
bool jsvm_set_pinned_app(const char *path);
#endif

// ---- host hooks: implement these in your sketch -----------------------------
// These are what sys.fps(), sys.backlight() and sys.battery() call.

// Frames pushed to the panel in the last second; return 0 if you don't track it.
uint32_t jsvm_host_fps();

// Set panel backlight, 0-100. Already clamped to that range.
void jsvm_host_backlight(uint8_t percent);

// Battery voltage, or NAN when unavailable (no divider fitted, or the ADC is
// busy). NAN surfaces to scripts as null.
float jsvm_host_battery();
