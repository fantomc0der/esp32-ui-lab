# lv-binding-js-esp32

Script an [LVGL 9](https://lvgl.io) UI in JavaScript on the ESP32, using a QuickJS-ng engine embedded in the firmware. The UI becomes a text file you load at runtime instead of code you compile in.

> Independent implementation, **not** a port of and not affiliated with [lvgl/lv_binding_js](https://github.com/lvgl/lv_binding_js). That project targets embedded Linux and depends on libuv, curl, and txiki.js, none of which fit a FreeRTOS microcontroller. This library keeps the same idea (engine in firmware, hand-written bindings over LVGL's C API, scripts as data) and drops the POSIX layers. The full comparison is in [`docs/lang-js/design-rationale.md`](../../docs/lang-js/design-rationale.md).

**There is no JSX, no React, and no virtual DOM here** — that is the other half of what was dropped. Scripts are plain imperative JavaScript that build widgets directly:

```js
const btn = lv.button(lv.screen(), { text: "Scan", w: 84, align: "top-left" });
btn.on("click", () => console.log("tapped"));
```

## What scripts get

| | |
|---|---|
| Widgets | `lv.obj`, `lv.button`, `lv.label`, `lv.slider`, `lv.switch`, `lv.arc`, `lv.list`, `lv.chart`, `lv.tabview`, plus `lv.screen()` for the root |
| Props | size (px, `"50%"`, `"content"`), `align`/`x`/`y`, `flex`/`flexAlign`, text, colors, font, `range`, `value`, padding, borders, and a few per-widget extras |
| Methods | `.set()`, `.on()` (click, change, press, pressing), `.value()`, `.bounds()`, `.clean()`, plus `.add()`, `.addTab()`, `.push()` on the widgets that take them |
| Timers | `lv.timer(ms, fn)`, returning a handle with `.stop()` |
| Device | `sys` (heap, battery, uptime, fps, backlight, chip info), `wifi.scan()`, `console.log` |
| Language | full ES2023 including closures, `JSON`, `Promise`, `async`/`await` |

Full reference with every prop and its accepted values: [`docs/lang-js/binding-api.md`](../../docs/lang-js/binding-api.md).

Anything not in that list has to be added to the binding layer in C before a script can reach it — that is the cost of a hand-written binding rather than a generated one. Adding a widget is about three lines; [`docs/lang-js/architecture.md`](../../docs/lang-js/architecture.md) has the recipes.

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

Loading that script from an SD card or flash partition, hot-reloading it on a button press, and exposing a serial REPL are all host policy and deliberately live outside this library. [`../JsHost/`](../JsHost/README.md) is a complete worked example of all three.

## The rule that matters

Every `jsvm_*` call must happen on the task that runs `lv_timer_handler()`, and no other task may touch the VM or LVGL. That is what makes the design lock-free, and breaking it corrupts memory rather than failing cleanly. Asynchronous network libraries (ESPAsyncWebServer, most MQTT clients) invoke callbacks on their own tasks: queue the payload there and drain it from the LVGL task.

Internals, ownership rules, and how to extend the binding surface: [`docs/lang-js/architecture.md`](../../docs/lang-js/architecture.md).
