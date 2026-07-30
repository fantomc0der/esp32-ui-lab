# Binding API

The JavaScript surface exposed by the [`lvgl-js-bindings`](../firmware/lvgl-js-bindings/README.md) library. Deliberately curated, not exhaustive: 17 widget makers plus around 30 module functions, covering the 20% of LVGL that delivers 80% of a small-panel UI. Everything runs on one FreeRTOS task (the same one that runs `lv_timer_handler`), so there is no locking anywhere and callbacks never race the renderer.

This page is the whole API, and everything below is what the firmware provides. There is also an optional layer above it: [`ui-runtime.md`](ui-runtime.md) describes a JSX component model with hooks, written in JavaScript on top of these same calls and bundled into apps that ask for it. It changes how a layout is expressed, not what is available — every prop and every method here means the same thing under it.

## Globals

Six globals exist in every script: `lv`, `sys`, `fs`, `wifi`, `fetch`, and `console`. There is no module system and no `import`; one script file is the whole app, and `fs` is how it reaches storage.

The full modern language is available (closures, template literals, BigInt, JSON, `Promise`, `async`/`await` — the host pumps QuickJS's job queue every loop, so promise reactions actually run). There is no `setTimeout`; use `lv.timer`.

## lv — widgets

| Call | Notes |
|---|---|
| `lv.screen()` | the active screen as a widget handle |
| `lv.size()` | `{w, h}` of the display in pixels — read it once and compute from it, for the cases percentages cannot express |
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
| `lv.bar(parent, props?)` | progress bar: a slider with no knob and no touch. `range`, `value` |
| `lv.checkbox(parent, props?)` | `text` is its label, `value` is the boolean |
| `lv.roller(parent, props?)` | scrolling picker; `options`, `visibleRows`, `infinite`. `.value()` is the selected **index** |
| `lv.dropdown(parent, props?)` | drop-down picker; `options`. `.value()` is the selected **index** |
| `lv.spinner(parent, props?)` | indeterminate busy indicator; `duration` (ms per turn), `sweep` (arc degrees) |
| `lv.led(parent, props?)` | status lamp; `color` is the lamp colour, `value` is on/off, `brightness` is 0–255 |
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
| `range` | `[min, max]` for slider/bar/arc, Y axis for chart |
| `value` | number for slider/bar/arc, boolean for switch/checkbox/led, selected index for roller/dropdown, text for textarea |
| `options` | roller and dropdown: an array of strings (or one `"\n"`-separated string) |
| `visibleRows`, `infinite` | roller only: how many rows show, and whether it wraps around |
| `brightness` | led only, 0–255 |
| `duration`, `sweep` | spinner only: milliseconds per turn, and the arc's length in degrees |
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

Three tools, in the order you should reach for them.

**Percentages and `flex`, for anything whose parent is already sized.** This is most of a layout, and it needs no arithmetic:

```js
const bar = lv.obj(lv.screen(), { w: "100%", h: "20%", flex: "row", flexAlign: ["evenly", "center"] });
lv.label(bar, { text: "left" });
lv.label(bar, { text: "right" });
```

**Alignment, for anything that should hang off an edge or the middle.** An element aligned `bottom-mid` stays at the bottom of any panel with no size involved, so the extra space on a larger display goes into the margin instead of into a gap. The weather app is laid out entirely this way and reads no dimensions at all ([`app/src/weather.jsx`](../app/src/weather.jsx), or [its pre-port original](../tools/fixtures/weather-imperative.js) for the same layout in plain `lv` calls).

**`lv.size()`, for the decisions a percentage cannot express.** It returns `{w, h}` of the display. Read it once at startup, since a panel does not resize:

```js
const S = lv.size();
// A header is one line of a fixed-size font, so its height is a constant and the
// list takes whatever is left — not "74%", which is only right on one panel.
const list = lv.list(scr, { w: "94%", h: S.h - 44, align: "bottom-mid" });
// How much history to show, rather than the same history stretched wider.
const chart = lv.chart(box, { w: "100%", h: "70%", points: Math.max(20, (S.w / 8) | 0) });
```

The pattern worth copying from [`app/app.js`](../app/app.js) and [`apps/vitals.js`](../app/apps/vitals.js): mixing units on purpose. Percentages for what scales, pixel constants for what holds text, and a computed remainder for whatever fills the rest. A layout expressed purely in percentages looks resolution-independent and is not, because the text inside it does not scale with the boxes.

`lv.size()` reports the *display*, not whatever you are putting children into. The two are the same for a screen with no padding, which is why subtracting from `S.h` works above, but for a padded or nested parent use `parent.bounds()` instead — it returns that parent's actual content area, after padding and borders, and it is the honest number when the container's own size came from a percentage. `apps/vitals.js` sizes its arc that way, because "40% of the tab" and "what fits inside the 40% column" differ by the padding and border between them.

Two things that stay in pixels no matter what:

- **Fonts do not scale.** The sizes are fixed bitmaps compiled into the firmware, so text is the same height on a 172px panel and a 320px one. Anything sized to fit text is therefore a pixel constant, and a percentage there will clip on a small panel or float in space on a large one.
- **The firmware's corner button is 34px in the bottom-right** of every screen (see [Apps and getting back](#apps-and-getting-back)). A control placed under it is unreachable, so leave ~40px clear there — the wifi app sizes its keyboard `S.w - CORNER` for exactly this reason.

`selftest.js` deliberately keeps fixed sizes throughout: its geometry is test fixtures with expected values, not a layout.

### One ordering rule, for the pickers

`options` and `value` in the same call work as you would expect — `lv.roller(p, { options: ["a", "b", "c"], value: 2 })` selects `"c"`. That is not free: props are applied in a fixed order and `value` is read before `options`, so the selection is applied a second time once the list exists. Without that, LVGL would clamp a selection made against an empty list to zero. Worth knowing only because it is the one place where a prop is not simply written once.

### Widget methods

- `.set(props)` — apply props, returns the widget (chainable)
- `.on(event, fn)` — `"click"`, `"change"`, `"pressing"`, `"press"`, `"longpress"`; returns the widget. While an input device is dispatching, the callback is `fn(widget, x, y)` with the current touch point in screen coordinates; otherwise just `fn(widget)`. The coordinates come from the input device rather than from this event, so a `.value(n)` made inside a click handler raises a `"change"` that carries the click's coordinates — treat `x, y` as "where the finger is", not "where this event happened". `"longpress"` fires while the finger is still down, and LVGL still sends `"click"` when it lifts — a long-press gesture that must not also count as a tap has to claim the click itself, which is what [`app/app.js`](../app/app.js) does with a flag it clears on `"press"`
- `.value()` / `.value(n)` — get/set for slider, bar, arc, switch, checkbox, led, and the two pickers (where it is the selected index, so a script indexes its own `options` array rather than parsing a string back)
- `.add(text)` — lists only; returns the row's button handle
- `.addTab(name)` — tabviews only; returns the tab's content container
- `.push(n)` — charts only; appends to the series, shifting left when full
- `.clean()` — deletes all children (their callbacks are released via the DELETE hooks)
- `.delete()` — deletes this one widget and its subtree, leaving its siblings alone. Same hazard as `.clean()`: deleting the widget LVGL is currently dispatching an event to is not safe, so defer it a tick. Throws on the screen itself
- `.index()` / `.index(n)` — read or set this widget's position among its parent's children. Moving an existing widget is how a reordered list keeps the widgets it already had
- `.bounds()` — `{ x, y, w, h }` of the content area in screen coordinates; combine with the `x, y` from a pointer event to place children under a finger

Every method sits on one shared prototype, so the ones marked "lists only" and the like exist on every handle and refuse at the point of the call: `lv.label(p, {}).push(3)` throws `push() only works on lv.chart widgets`. [`tools/check-js-api.mjs`](../tools/check-js-api.mjs) catches that on the PC where it can see what the receiver is — a `const` bound straight to a maker call, and chains through `.set()` and `.on()`. A widget from `.add()` or `.addTab()`, a function parameter or anything reassigned is left to the board, so the failure above is still reachable; the check narrows the gap rather than closing it. A line that means to call the wrong method (the misuse section of [`app/selftest.js`](../app/selftest.js)) opts out with a `// check-js-api: wrong kind on purpose` comment.

## fs — files

Paths are absolute. An unprefixed path uses the SD card, falling back to the flash partition when no card is fitted; a `flash:` prefix always means flash. Reads block the UI task and are capped at 256 KB, so this is for config, logs, and cached data rather than large media.

`fs.read(path)` returns the contents or `null`; `fs.write(path, text)` and `fs.append(path, text)` return a boolean; `fs.exists`, `fs.remove`, `fs.mkdir`, and `fs.isDir` do what they say; `fs.list(dir)` returns an array of bare names (or `null` if it isn't a directory); `fs.available()` tells you whether any storage is mounted, so a script can degrade instead of throwing.

## fetch — HTTP

`fetch(url)` returns a `Promise` resolving to `{ status, ok, body }`, where `body` is the raw text. It rejects on transport failure, and throws immediately if there is no connection or another request is already in flight (one at a time). "One at a time" spans app switches: an abandoned request keeps the radio until it times out, so the first `fetch()` of a freshly launched app can throw "the previous fetch is still finishing" and is worth retrying a second later. HTTPS works, without certificate validation. Bodies are capped at 128 KB.

The request runs on a worker task, so a slow response never freezes rendering or touch, and your callback still arrives on the normal task like every other callback.

```js
const res = await fetch("https://api.example.com/thing");
if (res.ok) label.set({ text: JSON.parse(res.body).value });
```

## wifi

- `wifi.status()` → `{ connected, ssid, ip, rssi, saved }`. Deliberately never returns the password.
- `wifi.save(ssid, password)` stores the credentials in NVS and connects. They survive reboots and reflashes, and the board rejoins on its own afterwards, including after a router restart.
- `wifi.connect()` retries with what's stored; `wifi.forget()` clears it.
- `wifi.scan(fn)` — starts an async scan and returns `true`; `fn(nets)` is called once with `[{ ssid, rssi, open }, ...]` or `null` if the scan failed. Returns `false` (and never calls `fn`) if a scan is already running. The UI keeps rendering during the scan.

Credentials are write-only by design: a script can set them but no API hands them back, so a script from a card can't read your network password off the device.

## sys

- `sys.heap()` → `{ internal, psram }` free bytes
- `sys.battery()` → volts, or `null` when the ADC is unavailable (GPIO12 is ADC2, which the WiFi driver arbitrates; also `null`-prone while the radio is busy)
- `sys.uptime()` → milliseconds since boot
- `sys.fps()` → panel flushes over the last 1 s window (host-measured)
- `sys.backlight(pct)` → LEDC PWM, clamped 0–100, hardware floor keeps the panel faintly visible. A setter only: unlike every other `sys` reading it has no getter form, so there is no `sys.backlight()`. `pct` must genuinely be a number, and everything else throws a `TypeError` rather than being converted, including `null`, `undefined`, `NaN`, booleans, objects and strings (`sys.backlight("80")` throws; convert it yourself). That is stricter than the rest of the API on purpose. JavaScript sends `null`, `false` and `""` to `0`, `0` is the one level indistinguishable from a crashed board, and a script that reaches it by accident gets no error and no clue: `sys.backlight(cfg.brightness)` against a config with `"brightness": null` used to dim the panel silently. Asking for `0` deliberately still works, it just has to be `0`. `Infinity` clamps to 100. Nothing reads the level back, because the board's PWM is write-only: it is whatever was last written, starting from the brightness the board boots at rather than from anything a script chose.
- `sys.info()` → `{ model, rev, cores, mhz, flashMB, psramMB, lvgl, quickjs }`
- `sys.launch(path)` → asks the firmware to run a different script. It returns immediately and your app keeps running until the current call finishes; the switch happens after that. It cannot be synchronous, because tearing down the VM mid-call would free the function that is executing. Paths are limited to 127 bytes and a longer one throws a `RangeError` rather than being truncated into a request for some other file.
- `sys.pin(path)` → makes that script the one the board boots into, instead of the launcher. Stored in NVS, so it survives reboots and reflashes. Returns `true` on success; throws on a relative path. It does not switch apps: pair it with `sys.launch(path)` if you want both. Any existing pin is replaced without warning, and the path is not checked for existence or against the launcher, so pinning is as permissive as the caller.
- `sys.unpin()` → clears the pin; the launcher is the boot script again.
- `sys.pinned()` → the pinned path, or `null`. The path is whatever was pinned, which may name a script that has since been deleted.

## Apps and getting back

The board boots `/app.js`, the launcher, which lists `/apps/*.js`. Any script can hand over with `sys.launch("/apps/other.js")`.

You never have to provide a way back. The firmware draws a button on LVGL's top layer, above whatever your app draws and outside the widget tree it can delete, and a long-press of BOOT does the same thing in hardware. It sits in the bottom-right corner, so leave that corner clear if you're placing something full-width along the bottom.

That corner holds at most one control, and the firmware picks it:

- **home** — back to the launcher, on a board with no pin
- **back** — to the pinned app, when a pin is set and you are somewhere else. It goes to the pin rather than to wherever you came from, so on a pinned board an app you opened *from* the launcher offers a back arrow that lands on the pinned app instead of returning you to the launcher
- **Wi-Fi** — to `/apps/wifi.js`, when your app has asked about the network and cannot get one
- nothing at all, when you are already where the button would take you

You never opt into the Wi-Fi one: calling `fetch()` or `wifi.status()` is what tells the firmware your app cares about the network, and an app that never mentions the network never shows it. It appears when nothing is set up, when the saved password is being rejected, and when the saved network has been missing for about a minute — the three cases where someone has to go and fix something.

It stays hidden for failures that retrying can still resolve: a dropped link, a router mid-reboot, the first few attempts at anything. Those are yours to report, and "offline" is the honest thing to say — you can tell them apart with `wifi.status().error` and `.attempts`.

## Pinning one app

A pin does two separate things, and it is worth reading them as two, because only the first is a lock of any kind:

1. **It changes what boots.** The board runs that script instead of the launcher.
2. **It changes what the firmware draws over a running app.** While the pinned app is what's running, the corner is empty, so nothing on screen refers to a launcher that is not part of the product. That corner is yours again, and a pinned app should provide whatever navigation it needs itself.

What a pin is *not* is a restriction on which apps can run. The launcher is still there, still reachable, and still switches apps. So "appliance" describes the boot path and the clean corner, not a kiosk that has been locked down: there is no mode in this firmware where only one script can execute.

The two things the firmware still draws over a pinned app are the ones a pinned board would otherwise have no answer for. If your pinned app wants a network it cannot get (none configured, a rejected password, a network that has gone away) the Wi-Fi button appears; tapping it opens the setup app, and from there the corner shows a back arrow to your app. Both are gone again as soon as the pinned app is what's running and has a network.

### Reaching a pinned board, and what the launcher does there

A long-press of BOOT (≥ 700 ms) always opens the launcher, whatever is pinned. It is the only route in on a pinned board, since the corner draws nothing, and it is deliberately not advertised on screen: discovering it means reading this page, the board's README, or the source.

Once you are in the launcher, both gestures on an app row stay live, and they do different things:

| Gesture on a row | Effect | Pin afterwards |
|---|---|---|
| Tap | runs that app now | unchanged |
| Long-press an unpinned row | **repins to that app**, silently replacing the previous pin | the row you held |
| Long-press the highlighted row | releases the pin | none |

Two consequences of that middle row are easy to meet by accident. Repinning is a plain overwrite with no confirmation: the old pin is discarded, the highlight and the `pinned:` label move, and nothing asks. And a tap does not repin, so an app you open this way is running on a board whose pin points elsewhere, which is the one case where the corner shows a back arrow that does not go back: it goes to the pinned app, per [the corner rules above](#apps-and-getting-back). Getting to the launcher from there is another BOOT long-press.

Over serial, `pin [path]` and `unpin` do the same thing, with one difference worth knowing: `pin` refuses to pin the launcher itself, while `sys.pin()` from a script does not check. Pinning `/app.js` would leave every app showing a back arrow to the launcher rather than a home icon, which works but reads oddly.

A pin that names a missing or broken script costs you the appliance behaviour, not the board: the load fails, and the firmware falls back to the launcher exactly as it does for any other app. A pin that no `/apps` row accounts for still gets a row of its own there, marked `(missing)` when the file is genuinely gone, because a pin you cannot see is a pin you cannot release without a serial cable.

One thing to know when building multi-screen apps: rebuilding the screen from inside a click handler deletes the widget LVGL is currently dispatching to. Defer it by a tick instead:

```js
const next = fn => { const t = lv.timer(20, () => { t.stop(); fn(); }); };
button.on("click", () => next(showOtherScreen));
```

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
