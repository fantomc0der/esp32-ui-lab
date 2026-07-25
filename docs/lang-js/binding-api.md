# JsHost binding API

The JavaScript surface exposed by `lang-js/JsHost/js_bindings.cpp`. Deliberately curated, not exhaustive: ~15 functions covering the 20% of LVGL that delivers 80% of a small-panel UI. Everything runs on one FreeRTOS task (the same one that runs `lv_timer_handler`), so there is no locking anywhere and callbacks never race the renderer.

## Globals

Four objects exist in every script: `lv`, `sys`, `wifi`, `console`. There is no module system, no `import`, no filesystem access from JS. One script file is the whole app.

## lv — widgets

| Call | Notes |
|---|---|
| `lv.screen()` | the active screen as a widget handle |
| `lv.obj(parent, props?)` | plain container |
| `lv.button(parent, props?)` | `text` prop creates/updates a centered child label |
| `lv.label(parent, props?)` | |
| `lv.slider(parent, props?)` | `range: [min, max]`, `value` |
| `lv.switch(parent, props?)` | boolean `value` |
| `lv.arc(parent, props?)` | `range`, `value` |
| `lv.list(parent, props?)` | rows via `list.add(text)` |
| `lv.timer(ms, fn)` | LVGL timer; returns a handle with `.stop()`; 10 ms floor |

### Props

Accepted at creation and via `.set(props)`. Unknown keys are ignored (scripts should degrade, not throw, on older firmware).

| Key | Value |
|---|---|
| `w`, `h` | pixels, or `"content"` for size-to-content |
| `align` | `center`, `top-left`, `top-mid`, `top-right`, `left-mid`, `right-mid`, `bottom-left`, `bottom-mid`, `bottom-right`; `x`/`y` become offsets from it |
| `x`, `y` | offsets with `align`, absolute position without |
| `text` | labels and buttons |
| `bg`, `color` | background / text color: `"#RRGGBB"` string or `0xRRGGBB` number |
| `font` | `14`, `16`, or `20` (the three compiled-in montserrat sizes) |
| `range` | `[min, max]` for slider/arc |
| `value` | number for slider/arc, boolean for switch |
| `pad`, `radius` | style shorthands, pixels |
| `scroll` | `false` removes the scrollable flag |
| `hidden` | boolean |

### Widget methods

- `.set(props)` — apply props, returns the widget (chainable)
- `.on(event, fn)` — `"click"`, `"change"`, `"pressing"`; `fn(widget)` receives the widget handle; returns the widget
- `.value()` / `.value(n)` — get/set for slider, arc, switch
- `.add(text)` — lists only; returns the row's button handle

## sys

- `sys.heap()` → `{ internal, psram }` free bytes
- `sys.battery()` → volts, or `null` when the ADC is unavailable (GPIO12 is ADC2, which the WiFi driver arbitrates; also `null`-prone while the radio is busy)
- `sys.uptime()` → milliseconds since boot
- `sys.fps()` → panel flushes over the last 1 s window (host-measured)
- `sys.backlight(pct)` → LEDC PWM, clamped 0–100, hardware floor keeps the panel faintly visible
- `sys.info()` → `{ model, rev, cores, mhz, flashMB, psramMB, lvgl, quickjs }`

## wifi

- `wifi.scan(fn)` — starts an async scan and returns `true`; `fn(nets)` is called once with `[{ ssid, rssi }, ...]` sorted by the driver, or `null` if the scan failed. Returns `false` (and never calls `fn`) if a scan is already running. The UI keeps rendering during the scan.

## console

- `console.log(...args)` / `console.error(...args)` → USB serial, space-separated.

## Ownership and GC rules (why scripts can't crash the firmware)

This is the correctness core of the binding layer; the invariants, in script-author terms:

- Every callback you pass to `.on()` or `lv.timer()` is retained by the C side (`JS_DupValue`) and released exactly once. Event callbacks are released by an `LV_EVENT_DELETE` hook on their widget, timer callbacks when `.stop()` is called or the app is torn down.
- A callback that throws is reported over serial (message + stack) and the app keeps running; exceptions never unwind into C.
- Widget handles are weak: LVGL owns the widget tree, JS holds bare pointers. v1 exposes no `delete`, so a handle can only dangle across a reload, and a reload destroys the whole JS context first — stale handles cannot survive it.
- Teardown order on reload is fixed: JS timers → `lv_obj_clean(screen)` (fires the DELETE hooks) → screen-level bindings → context → runtime. Scripts don't clean up after themselves; there is nothing they need to (or can) do at teardown.
- The JS heap lives in PSRAM (measured: the engine costs ~80 KB of PSRAM and ~350 bytes of internal RAM at rest); internal RAM stays reserved for draw buffers and the radio.
