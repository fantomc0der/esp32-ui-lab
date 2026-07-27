# js-host binding API

The JavaScript surface exposed by the [`lvgl-js-bindings`](../../lang-js/lvgl-js-bindings/README.md) library. Deliberately curated, not exhaustive: about twenty functions covering the 20% of LVGL that delivers 80% of a small-panel UI. Everything runs on one FreeRTOS task (the same one that runs `lv_timer_handler`), so there is no locking anywhere and callbacks never race the renderer.

## Globals

Four objects exist in every script: `lv`, `sys`, `wifi`, `console`. There is no module system, no `import`, no filesystem access from JS. One script file is the whole app.

The full modern language is available (closures, template literals, BigInt, JSON, `Promise`, `async`/`await` — the host pumps QuickJS's job queue every loop, so promise reactions actually run). There is no `setTimeout`; use `lv.timer`.

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
| `lv.chart(parent, props?)` | single-series line chart in shift mode, point dots hidden; feed it with `.push(n)` |
| `lv.tabview(parent, props?)` | swipeable/tappable tabs; `bar` prop sets tab-bar height, `.addTab(name)` returns the tab's content container |
| `lv.textarea(parent, props?)` | text field; `placeholder`, `password` (masks input), `oneLine`, `maxLength`. `.value()` reads and writes its text |
| `lv.keyboard(parent, props?)` | on-screen keyboard; `.target(textarea)` routes typing into a field, and it emits `ready`/`cancel` for its tick and cross keys |
| `lv.timer(ms, fn)` | LVGL timer; returns a handle with `.stop()`; 10 ms floor |

### Props

Accepted at creation and via `.set(props)`. Unknown keys are ignored (scripts should degrade, not throw, on older firmware).

| Key | Value |
|---|---|
| `w`, `h` | pixels (`120`), a percentage of the parent's content area (`"50%"`), or `"content"` to shrink-wrap children |
| `flex` | `"row"`, `"column"`, `"row-wrap"`, `"column-wrap"` — children position themselves instead of being placed by hand |
| `flexAlign` | `"center"`, or `["main", "cross"]`; each of `start`, `end`, `center`, `between`, `around`, `evenly` |
| `align` | `center`, `top-left`, `top-mid`, `top-right`, `left-mid`, `right-mid`, `bottom-left`, `bottom-mid`, `bottom-right`; `x`/`y` become offsets from it |
| `x`, `y` | offsets with `align`, absolute position without |
| `text` | labels and buttons |
| `bg`, `color` | background / text color: `"#RRGGBB"` string or `0xRRGGBB` number |
| `font` | `14`, `16`, `20`, `28`, or `40` (the compiled-in montserrat sizes) |
| `range` | `[min, max]` for slider/arc, Y axis for chart |
| `value` | number for slider/arc, boolean for switch |
| `pad`, `radius` | style shorthands, pixels |
| `border`, `borderColor` | border width (pixels) and color |
| `scroll` | `false` removes the scrollable flag |
| `clickable` | boolean, adds/removes the clickable flag (plain `lv.obj` isn't clickable by default) |
| `hidden` | boolean |
| `rotation`, `angles`, `knob` | arc only: start rotation, `[bgStart, bgEnd]` angles, and `knob: false` turns the arc into a pure indicator (no knob, not touchable) |
| `points`, `divs` | chart only: point count, `[hDiv, vDiv]` grid lines |
| `seriesColor` | chart only, **at creation only**: the line color. Ignored by `.set()`, since the series is created with the widget. |
| `bar` | tabview only: tab-bar height in pixels |

### Writing for more than one screen

Pixel coordinates are the quickest way to lay out a UI you only ever run on one panel, and that is what [`app/app.js`](../../lang-js/app/app.js) does. To make a script survive a different resolution, size with percentages and let `flex` place the children:

```js
const bar = lv.obj(lv.screen(), { w: "100%", h: "20%", flex: "row", flexAlign: ["evenly", "center"] });
lv.label(bar, { text: "left" });
lv.label(bar, { text: "right" });
```

Fonts do not scale: the three sizes are fixed bitmaps compiled into the firmware, so text stays the same pixel height on a larger display.

### Widget methods

- `.set(props)` — apply props, returns the widget (chainable)
- `.on(event, fn)` — `"click"`, `"change"`, `"pressing"`, `"press"`, `"longpress"`; returns the widget. When a pointer drove the event the callback is `fn(widget, x, y)` with the touch point in screen coordinates; otherwise just `fn(widget)`. `"longpress"` fires while the finger is still down, and LVGL still sends `"click"` when it lifts — a long-press gesture that must not also count as a tap has to claim the click itself, which is what [`app/app.js`](../../lang-js/app/app.js) does with a flag it clears on `"press"`
- `.value()` / `.value(n)` — get/set for slider, arc, switch
- `.add(text)` — lists only; returns the row's button handle
- `.addTab(name)` — tabviews only; returns the tab's content container
- `.push(n)` — charts only; appends to the series, shifting left when full
- `.clean()` — deletes all children (their callbacks are released via the DELETE hooks)
- `.bounds()` — `{ x, y, w, h }` of the content area in screen coordinates; combine with the `x, y` from a pointer event to place children under a finger

## fs — files

Paths are absolute. An unprefixed path uses the SD card, falling back to the flash partition when no card is fitted; a `flash:` prefix always means flash. Reads block the UI task and are capped at 256 KB, so this is for config, logs, and cached data rather than large media.

`fs.read(path)` returns the contents or `null`; `fs.write(path, text)` and `fs.append(path, text)` return a boolean; `fs.exists`, `fs.remove`, `fs.mkdir`, and `fs.isDir` do what they say; `fs.list(dir)` returns an array of bare names (or `null` if it isn't a directory); `fs.available()` tells you whether any storage is mounted, so a script can degrade instead of throwing.

## fetch — HTTP

`fetch(url)` returns a `Promise` resolving to `{ status, ok, body }`, where `body` is the raw text. It rejects on transport failure, and throws immediately if there is no connection or another request is already in flight (one at a time). HTTPS works, without certificate validation. Bodies are capped at 128 KB.

The request runs on a worker task, so a slow response never freezes rendering or touch, and your callback still arrives on the normal task like every other callback.

```js
const res = await fetch("https://api.example.com/thing");
if (res.ok) label.set({ text: JSON.parse(res.body).value });
```

## wifi

- `wifi.status()` → `{ connected, ssid, ip, rssi, saved }`. Deliberately never returns the password.
- `wifi.save(ssid, password)` stores the credentials in NVS and connects. They survive reboots and reflashes, and the board rejoins on its own afterwards, including after a router restart.
- `wifi.connect()` retries with what's stored; `wifi.forget()` clears it.
- `wifi.scan(fn)` — async scan; `fn(nets)` gets `[{ ssid, rssi, open }, ...]` or `null`. Returns `false` if a scan is already running.

Credentials are write-only by design: a script can set them but no API hands them back, so a script from a card can't read your network password off the device.

## sys

- `sys.heap()` → `{ internal, psram }` free bytes
- `sys.battery()` → volts, or `null` when the ADC is unavailable (GPIO12 is ADC2, which the WiFi driver arbitrates; also `null`-prone while the radio is busy)
- `sys.uptime()` → milliseconds since boot
- `sys.fps()` → panel flushes over the last 1 s window (host-measured)
- `sys.backlight(pct)` → LEDC PWM, clamped 0–100, hardware floor keeps the panel faintly visible
- `sys.info()` → `{ model, rev, cores, mhz, flashMB, psramMB, lvgl, quickjs }`
- `sys.launch(path)` → asks the firmware to run a different script. It returns immediately and your app keeps running until the current call finishes; the switch happens after that. It cannot be synchronous, because tearing down the VM mid-call would free the function that is executing.
- `sys.pin(path)` → makes that script the one the board boots into, instead of the launcher. Stored in NVS, so it survives reboots and reflashes. Returns `true` on success; throws on a relative path. It does not switch apps — pair it with `sys.launch(path)` if you want both.
- `sys.unpin()` → clears the pin; the launcher is the boot script again.
- `sys.pinned()` → the pinned path, or `null`. The path is whatever was pinned, which may name a script that has since been deleted.

## Apps and getting back

The board boots `/app.js`, the launcher, which lists `/apps/*.js`. Any script can hand over with `sys.launch("/apps/other.js")`.

You never have to provide a way back. The firmware draws a button on LVGL's top layer, above whatever your app draws and outside the widget tree it can delete, and a long-press of BOOT does the same thing in hardware. It sits in the bottom-right corner, so leave that corner clear if you're placing something full-width along the bottom.

That corner holds at most one control, and the firmware picks it:

- **home** — back to the launcher, on a board with no pin
- **back** — back to the pinned app, when a pin is set and you are somewhere else
- **Wi-Fi** — to `/apps/wifi.js`, when your app has asked about the network and cannot get one
- nothing at all, when you are already where the button would take you

You never opt into the Wi-Fi one: calling `fetch()` or `wifi.status()` is what tells the firmware your app cares about the network, and an app that never mentions the network never shows it. It appears when nothing is set up, when the saved password is being rejected, and when the saved network has been missing for about a minute — the three cases where someone has to go and fix something.

It stays hidden for failures that retrying can still resolve: a dropped link, a router mid-reboot, the first few attempts at anything. Those are yours to report, and "offline" is the honest thing to say — you can tell them apart with `wifi.status().error` and `.attempts`.

## Pinning one app

Pinning turns the board into an appliance: with a pin set, boot goes straight to that script and **the firmware draws nothing in the corner while that app is running**, so nothing on screen refers to a launcher that is no longer part of the product. That corner is yours again, and a pinned app should provide whatever navigation it needs itself.

The two exceptions are the ones a pinned board would otherwise have no answer for. If your pinned app wants a network it cannot get — none configured, a rejected password, a network that has gone away — the Wi-Fi button appears; tapping it opens the setup app, and from there the corner shows a back arrow to your app. Both are gone again as soon as the pinned app is what's running and has a network.

The escape hatch that remains is the hardware one: a long-press of BOOT (≥ 700 ms) always opens the launcher, whatever is pinned. That is how you reach a board you have pinned, and the launcher is where you release the pin — long-press an app row to pin it, long-press the highlighted row to unpin. Over serial, `pin [path]` and `unpin` do the same thing.

A pin that names a missing or broken script costs you the appliance behaviour, not the board: the load fails, and the firmware falls back to the launcher exactly as it does for any other app.

One thing to know when building multi-screen apps: rebuilding the screen from inside a click handler deletes the widget LVGL is currently dispatching to. Defer it by a tick instead, which is what [`apps/wifi.js`](../../lang-js/app/apps/wifi.js) does:

```js
const next = fn => { const t = lv.timer(20, () => { t.stop(); fn(); }); };
button.on("click", () => next(showOtherScreen));
```

## wifi

- `wifi.scan(fn)` — starts an async scan and returns `true`; `fn(nets)` is called once with `[{ ssid, rssi }, ...]` sorted by the driver, or `null` if the scan failed. Returns `false` (and never calls `fn`) if a scan is already running. The UI keeps rendering during the scan.

## console

- `console.log(...args)` / `console.error(...args)` → USB serial, space-separated.

## Ownership and GC rules (why scripts can't crash the firmware)

This is the correctness core of the binding layer; the invariants, in script-author terms:

- Every callback you pass to `.on()` or `lv.timer()` is retained by the C side (`JS_DupValue`) and released exactly once. Event callbacks are released by an `LV_EVENT_DELETE` hook on their widget, timer callbacks when `.stop()` is called or the app is torn down.
- A callback that throws is reported over serial (message + stack) and the app keeps running; exceptions never unwind into C.
- A callback may destroy what invoked it. Stopping your own timer (`const t = lv.timer(200, () => t.stop())`, the one-shot idiom) and calling `.clean()` on a container that holds the widget you are handling are both safe: the binding layer holds its own references for the duration of the call.
- Widget handles are weak: LVGL owns the widget tree, JS holds bare pointers. A handle kept across a `.clean()` of its container refers to a deleted widget, and using it throws `TypeError: widget has been deleted` rather than corrupting anything. Reloads are not a concern either way, since teardown destroys every handle along with the context.
- Teardown order on reload is fixed: JS timers → `lv_obj_clean(screen)` (fires the DELETE hooks) → screen-level bindings → context → runtime. Scripts don't clean up after themselves; there is nothing they need to (or can) do at teardown.
- The JS heap lives in PSRAM (measured: the engine costs ~80 KB of PSRAM and ~350 bytes of internal RAM at rest); internal RAM stays reserved for draw buffers and the radio.
