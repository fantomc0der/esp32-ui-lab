# Plan: JavaScript-scripted LVGL on the ESP32-S3-Touch-LCD-1.47

Goal: write UI logic for this board in JavaScript, edited without recompiling
firmware — the *idea* of lvgljs, re-derived for a FreeRTOS MCU instead of
embedded Linux.

## Why not port lvgljs directly

lvgljs = QuickJS + **libuv + curl + txiki.js** + a React/JSX virtual-DOM layer.
The bold parts assume POSIX (processes, fds, BSD sockets) and are the reason
its build targets are desktop/embedded-Linux only. Porting them to FreeRTOS is
a bigger project than the value they add here. What we keep is the core
architecture: **a JS engine in firmware + a binding layer over LVGL's C API +
scripts stored as data, not code.**

## Architecture

```
app.js  (SD card or FATFS partition — plain data, editable on a PC)
   │  loaded at boot / on reload command
   ▼
QuickJS-ng VM  (C library compiled into the sketch; heap in PSRAM)
   │  calls like lv.button(parent, {text:"Scan"}).on("click", fn)
   ▼
binding layer  (hand-written C glue: ~15 functions + event trampoline)
   ▼
LVGL 9  →  existing flush path  →  JD9853 panel      (unchanged, all C)
```

Threading rule: the VM runs only inside `loopTask` (same task as
`lv_timer_handler`), so no LVGL locking is ever needed. JS event callbacks are
dispatched from LVGL event callbacks via a C trampoline holding `JSValue`
function refs.

## Feasibility budget (this exact board)

| Resource | Cost | Have |
|---|---|---|
| Flash for QuickJS-ng | ~500–800 KB | 1.9 MB free in 3 MB app partition |
| JS heap | 256 KB–1 MB (PSRAM via `JSMallocFunctions`) | 8 MB PSRAM, ~8.1 MB free |
| Stack | QuickJS recurses; needs ~16–32 KB task stack | loopTask stack is configurable |
| CPU | interpreter only builds widgets / handles events; rendering stays C | fine at UI event rates |

## Phases

### Phase 1 — engine spike (the go/no-go gate)
1. Vendor QuickJS-ng (maintained fork) sources into the sketch as `.c` files
   (quickjs.c, libregexp.c, libunicode.c, cutils.c, xsum.c + headers).
2. Patch-out what Xtensa/newlib lacks (expected: none or trivial — no fork(),
   no signals used by the core interpreter).
3. `JS_Eval("1+1")` → print result over serial. Measure flash/RAM deltas.
4. Route `js_malloc` to `heap_caps_malloc(MALLOC_CAP_SPIRAM)`; set
   `JS_SetMaxStackSize` to fit the task stack; raise loopTask stack if needed.

**Exit criteria:** eval works, flash < 1 MB added, JS heap demonstrably in
PSRAM. If QuickJS fights the toolchain hard, fall back to JerryScript (smaller,
designed for MCUs) with the same binding design.

### Phase 2 — binding layer v1 (curated, not exhaustive)
Global `lv` object exposing roughly:
- `lv.screen()`, `lv.obj/button/label/slider/switch/arc/list(parent, props)`
- props: size, align, text, colors, font size (maps to the 3 compiled fonts)
- `widget.on("click"|"change"|"pressing", jsFn)` — C trampoline stores the
  `JSValue`, LVGL event cb re-enters the VM
- `lv.timer(ms, jsFn)` wrapping `lv_timer_create`
- `console.log` → Serial; `sys.heap()`, `sys.battery()`, `wifi.scan(cb)`
  (reusing the async-scan pattern already in the sketch)

GC discipline: every stored `JSValue` is `JS_DupValue`d and freed when the
widget is deleted (LVGL `LV_EVENT_DELETE` hook) — this is the main
correctness risk of the whole project; design it first.

### Phase 3 — script loading
1. Boot order: try `/sd/app.js` (SD_MMC, 4-bit, pins already in
   `board_pins.h`), else `/fatfs/app.js`, else a built-in fallback script in
   flash that shows "no app.js found" on the panel.
2. `flash.ps1 -PushScript` style helper later if wanted; v1 is "edit on PC,
   move card."

### Phase 4 — hot reload
- Long-press BOOT (button already wired in the sketch) → tear down JS context,
  `lv_obj_clean(screen)`, re-read app.js, rebuild. Iteration loop becomes:
  edit file → card in → press button. No toolchain on the inner loop.
- Stretch: reload over serial (`flash.ps1` pipes the file), then over WiFi.

### Phase 5 — proof + docs
- Reimplement the **System tab** (labels, slider, live values) as `app.js` —
  it exercises props, events, timers, and a native call, in ~40 lines of JS.
- Document the binding API and the GC ownership rules in `docs/`.

## Risks, honestly

- **QuickJS on Xtensa build friction** (Phase 1 exists to surface this early;
  JerryScript is the escape hatch).
- **JSValue lifetime bugs** — use-after-free between LVGL widget deletion and
  JS GC. Mitigated by the DELETE-hook ownership rule and by keeping v1's
  surface small.
- **Scope creep** — the lvgljs feature list (CSS, JSX, animations, images) is
  a siren. v1 is 15 functions; everything else earns its way in.
- **PSRAM latency** — JS heap in PSRAM is slower than SRAM; acceptable because
  JS runs at event rate, not pixel rate. If eval feels sluggish, move the
  engine's hot structures to internal RAM and only object storage to PSRAM.

## What this is not

Not a lvgljs port, not React, not npm. It's "MicroPython-style scripting, but
JavaScript, for this one board" — the 20% of lvgljs that delivers 80% of the
point: UI logic as editable data.
