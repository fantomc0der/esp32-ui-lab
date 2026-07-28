# lvgl-js-bindings

Script an [LVGL 9](https://lvgl.io) UI in JavaScript on the ESP32, using a QuickJS-ng engine embedded in the firmware. The UI becomes a text file you load at runtime instead of code you compile in.

> Independent implementation, **not** a port of and not affiliated with [lvgl/lv_binding_js](https://github.com/lvgl/lv_binding_js). That project targets embedded Linux and depends on libuv, curl, and txiki.js, none of which fit a FreeRTOS microcontroller. This library keeps the same idea (engine in firmware, hand-written bindings over LVGL's C API, scripts as data) and drops the POSIX layers. The full comparison is in [`docs/design-rationale.md`](../../docs/design-rationale.md).

**There is no JSX, no React, and no virtual DOM in this library** — that is the other half of what was dropped, and it stays dropped: the binding surface is imperative, and scripts build widgets directly.

```js
const btn = lv.button(lv.screen(), { text: "Scan", w: 84, align: "top-left" });
btn.on("click", () => console.log("tapped"));
```

A component model does exist a layer up, in [`app/lib/ui.js`](../../app/lib/ui.js): JSX with hooks and a reconciler, written in JavaScript against exactly the API below and bundled into the apps that ask for it. It costs this library nothing — no flash, no dependency, no second code path — which is the point of it living there rather than here. See [`docs/ui-runtime.md`](../../docs/ui-runtime.md).

## What scripts get

| | |
|---|---|
| Widgets | `lv.obj`, `lv.button`, `lv.label`, `lv.slider`, `lv.bar`, `lv.switch`, `lv.checkbox`, `lv.arc`, `lv.list`, `lv.chart`, `lv.tabview`, `lv.textarea`, `lv.keyboard`, `lv.roller`, `lv.dropdown`, `lv.spinner`, `lv.led`, plus `lv.screen()` for the root |
| Props | size (px, `"50%"`, `"content"`), `align`/`x`/`y`, `flex`/`flexAlign`, text, colors, font, `range`, `value`, padding, borders, and a few per-widget extras |
| Methods | `.set()`, `.on()` (click, change, press, pressing, longpress), `.value()`, `.bounds()`, `.clean()`, `.delete()`, `.index()`, plus `.add()`, `.addTab()`, `.push()`, `.target()` on the widgets that take them |
| Timers | `lv.timer(ms, fn)`, returning a handle with `.stop()` |
| Device | `sys` (heap, battery, uptime, fps, backlight, chip info, launch/pin), `wifi` (status, save, scan), `console.log` |
| Storage & network | `fs` (read, write, append, list, mkdir…) and `fetch(url)` returning a `Promise` |
| Language | full ES2023 including closures, `JSON`, `Promise`, `async`/`await` |

Full reference with every prop and its accepted values: [`docs/binding-api.md`](../../docs/binding-api.md).

Anything not in that list has to be added to the binding layer in C before a script can reach it — that is the cost of a hand-written binding rather than a generated one. Adding a widget is about three lines; [`docs/runtime-architecture.md`](../../docs/runtime-architecture.md) has the recipes.

## What's in src/

`jsvm_core.cpp` is the VM: QuickJS lifecycle, the PSRAM allocator, JSValue ownership, and teardown order. `bindings_lv.cpp`, `bindings_sys.cpp`, `bindings_fs.cpp`, and `bindings_wifi.cpp` are one global each. `jsvm_app.cpp` is the optional supervisor described under [Using it](#using-it). `js_bindings.h` is the whole public surface; `jsvm_internal.h` is what the core shares with the modules.

The invariant that keeps them separable: a binding module never stores a `JSValue`. Callbacks are handed to the core, which owns their lifetime. If you add a module, follow that.

`sys`, `fs` and `wifi` are optional — build with `-DJSVM_WITH_SYS=0`, `-DJSVM_WITH_FS=0` or `-DJSVM_WITH_WIFI=0` (in your sketch's `build_opt.h`) to leave any of them out entirely, which for wifi also drops `WiFi.h` from the firmware. `lv` and `console` are core and always present.

## Requirements

LVGL 9.x, the ESP32 Arduino core, PSRAM (the JS heap lives there), and the `quickjs-ng` library that ships alongside this one in [`../quickjs-ng/`](../quickjs-ng/README.md). Budget roughly 430 KB of flash for the engine and ~80 KB of PSRAM at rest.

Your sketch needs a `build_opt.h` containing `-D_GNU_SOURCE` and `-DNDEBUG` for the engine, and a task stack large enough for the interpreter's recursion (`SET_LOOP_TASK_STACK_SIZE(32 * 1024)`).

## Using it

Bring up your display and LVGL however your board requires, implement three hooks, then hand a script to `jsvm_start()`:

```cpp
#include <js_bindings.h>

// The only board-specific surface. Return NAN / 0 for anything you don't have.
uint32_t jsvm_host_fps()                  { return 0; }
void     jsvm_host_backlight(uint8_t pct) { analogWrite(BL_PIN, map(pct, 0, 100, 12, 255)); }
float    jsvm_host_battery()              { return NAN; }

SET_LOOP_TASK_STACK_SIZE(32 * 1024);

void setup() {
  Serial.begin(115200);
  // ... your display init: lv_init(), draw buffers, lv_display_create(),
  //     flush callback, pointer indev ...
  jsvm_start("lv.label(lv.screen(), { text: 'hello', align: 'center' })", "inline");
}

void loop() {
  lv_timer_handler();
  jsvm_pump();          // promise reactions; without this .then() never fires
  delay(5);
}
```

That is the low-level interface, and enough if your firmware ships one fixed script.

For a board that loads scripts off storage, the library also provides a supervisor: the boot chain (pinned app → SD `/app.js` → flash `/app.js` → a built-in fallback screen), a corner button on `lv_layer_top()` that no script can delete or cover, a long-press escape hatch, and a serial protocol for uploading a script and reloading it. None of that depends on the board, so it lives here rather than being copied into each sketch:

```cpp
void setup() {
  // ... display init, then mount your filesystems ...
  JsvmAppConfig cfg;
  cfg.sd = &SD_MMC;                 // either may be null
  cfg.flash = &FFat;
  cfg.launcher = "/app.js";
  cfg.wifi_app = "/apps/wifi.js";   // null to not offer network setup
  cfg.home_button_pin = 0;          // active low; -1 for none
  jsvm_app_begin(cfg);              // boots the first script
}

void loop() {
  lv_timer_handler();
  jsvm_app_service();               // replaces jsvm_pump()
  delay(5);
}
```

It is opt-in, and it has no privileged access: it is written against exactly the interface shown above it. Skip `jsvm_app_begin()` and you keep the low-level path. [`../boards/waveshare-s3-touch-147/`](../boards/waveshare-s3-touch-147/README.md) is a complete worked example, and 189 lines because of this split.

## The rule that matters

Every `jsvm_*` call must happen on the task that runs `lv_timer_handler()`, and no other task may touch the VM or LVGL. That is what makes the design lock-free, and breaking it corrupts memory rather than failing cleanly. Asynchronous network libraries (ESPAsyncWebServer, most MQTT clients) invoke callbacks on their own tasks: queue the payload there and drain it from the LVGL task.

Internals, ownership rules, and how to extend the binding surface: [`docs/runtime-architecture.md`](../../docs/runtime-architecture.md).
