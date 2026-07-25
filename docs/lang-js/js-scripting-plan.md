# Plan: JavaScript-scripted LVGL on the ESP32-S3-Touch-LCD-1.47

Goal: write UI logic for this board in JavaScript, edited without recompiling firmware — the *idea* of [lvgljs](https://lvgl.io/docs/open/integration/bindings/javascript), re-derived for a FreeRTOS MCU instead of embedded Linux.

**Status: all five phases complete and hardware-verified (2026-07-25), then extended.** QuickJS-ng v0.15.1 runs on the board (429 KB flash for the engine, JS heap in PSRAM — measurements and traps in [`engine-notes.md`](engine-notes.md)); the runtime firmware is [`lang-js/JsHost/`](../../lang-js/JsHost/README.md) with the binding surface documented in [`binding-api.md`](binding-api.md); the shipped app, [`lang-js/app/app.js`](../../lang-js/app/app.js), now recreates the full 4-tab C demo, not just the System tab this plan asked for. Verified end-to-end: fallback screen, ffat:/app.js loading, live REPL over serial, serial script upload (`app-begin`/`app-end`), async wifi.scan callback, and 5x hot-reload stress with no internal-RAM leak. The sections below are the original plan, kept as the design rationale.

## Cold-start context (read first)

What exists and is proven, as of 2026-07-25:

- **The board works and is fully verified.** `lang-c/WaveshareVitals/` is a 4-tab LVGL 9 demo confirmed on hardware: display init, colors, touch (taps/drags/slider), WiFi scan, native USB serial. It is the reference implementation for every hardware question.
- **Board:** Waveshare ESP32-S3-Touch-LCD-1.47 — ESP32-S3R8 (240 MHz dual LX7), 16 MB flash, 8 MB **OPI** PSRAM, 172×320 JD9853 display (SPI), AXS5106L touch (I2C `0x63`). Full pinout + traps: [`docs/hardware.md`](../hardware.md). Pin constants: `lang-c/WaveshareVitals/board_pins.h`.
- **Toolchain (verified versions):** arduino-cli at `C:\Users\micha\.local\bin\arduino-cli.exe`, esp32 core **3.3.11**, lvgl **9.5.0**, GFX Library for Arduino **1.6.7**. The board enumerates on the native USB port (was COM4; `VID_303A PID_1001`) — needs a **data** USB-C cable.
- **The FQBN that must be used** (wrong PSRAM setting = boot loop):
  ```
  esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=cdc,FlashMode=qio,USBMode=hwcdc
  ```
  `lang-c/flash.ps1` wraps compile/upload/monitor; adapt or copy it for the JS sketch. CLI details + scripted serial capture: [`docs/lang-c/build-and-flash.md`](../lang-c/build-and-flash.md).
- **Reusable C assets** (copy from `lang-c/WaveshareVitals/`, all hardware-proven): `board_pins.h`, `jd9853_panel.h` (JD9853 init blob — do not hand-edit register values), `axs5106l_touch.{h,cpp}` (touch mapping is `sx = raw_y; sy = raw_x`, measured on hardware — see [`docs/lang-c/touch.md`](../lang-c/touch.md)), `lv_conf.h` (a sketch-local copy is honored via `__has_include`), and the display/LVGL glue in `WaveshareVitals.ino`: flush cb (`lv_draw_sw_rgb565_swap` then `draw16bitBeRGBBitmap` — exactly one byte swap), draw buffers in internal DMA-capable RAM, tick cb, pointer indev cb, sleep-hint loop.
- **Key LVGL 9 gotcha:** `sizeof(lv_color_t)` is 3 and must never size RGB565 buffers; use an explicit 2 bytes/px. See the comments in `WaveshareVitals.ino`.
- **Layout convention:** per-language directories `lang-c/`, `lang-js/`; docs mirror this under `docs/lang-c/`, `docs/lang-js/`. New JS work goes in `lang-js/` (planned: `lang-js/JsSpike/` for Phase 1, then a runtime sketch + `lang-js/app/` for the shipped JS).
- **Prose style rule for all docs/commit bodies:** never hard-wrap; one continuous line per paragraph or list item.

## Why not port lvgljs directly

lvgljs = QuickJS + **libuv + curl + txiki.js** + a React/JSX virtual-DOM layer. The bold parts assume POSIX (processes, fds, BSD sockets) and are the reason its build targets are desktop/embedded-Linux only. Porting them to FreeRTOS is a bigger project than the value they add here. What we keep is the core architecture: **a JS engine in firmware + a binding layer over LVGL's C API + scripts stored as data, not code.**

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

Threading rule: the VM runs only inside `loopTask` (the same FreeRTOS task that calls `lv_timer_handler`), so no LVGL locking is ever needed. JS event callbacks are dispatched from LVGL event callbacks via a C trampoline holding `JSValue` function refs.

## Feasibility budget (this exact board)

| Resource | Cost | Have |
|---|---|---|
| Flash for QuickJS-ng | ~500–800 KB | ~1.9 MB free in the 3 MB app partition (C demo uses 1.26 MB) |
| JS heap | 256 KB–1 MB (PSRAM via `JSMallocFunctions`) | 8 MB PSRAM, ~8.1 MB free at runtime |
| Stack | QuickJS recurses; needs ~16–32 KB task stack | loopTask stack size is configurable |
| CPU | interpreter only builds widgets / handles events; rendering stays C | fine at UI event rates |

## Phases

### Phase 1 — engine spike (the go/no-go gate)

1. Create `lang-js/JsSpike/JsSpike.ino` — minimal sketch, Serial only, no display yet.
2. Vendor **QuickJS-ng** (the maintained QuickJS fork, github.com/quickjs-ng/quickjs) sources into the sketch folder as `.c`/`.h` files. Expected set: `quickjs.c`, `libregexp.c`, `libunicode.c`, `cutils.c`, `xsum.c` plus their headers and generated tables (`quickjs-atom.h`, `quickjs-opcode.h`, `libregexp-opcode.h`, `libunicode-table.h`, `list.h`, …) — check the repo's own build files for the authoritative list. Arduino compiles every `.c` in the sketch folder automatically.
3. Patch out anything Xtensa/newlib lacks (expected: little — the core interpreter uses no fork/signals; watch for atomics and `math.h` edge functions).
4. In `setup()`: create runtime + context, `JS_Eval` a trivial script (`"1+1"`, then something exercising closures and GC), print the result over serial.
5. Route the allocator to PSRAM: pass `JSMallocFunctions` backed by `heap_caps_malloc(MALLOC_CAP_SPIRAM)` into runtime creation; set `JS_SetMaxStackSize` well under the task stack; raise loopTask stack if needed (`SET_LOOP_TASK_STACK_SIZE()` macro in the esp32 core, or move the VM to a dedicated task later).
6. Measure and record: flash delta, internal-RAM delta, PSRAM usage, eval time.

**Exit criteria:** eval works on hardware, flash cost < 1 MB, JS heap demonstrably in PSRAM. **Fallback if QuickJS fights the toolchain hard:** JerryScript (smaller, MCU-first) with the same binding design.

### Phase 2 — binding layer v1 (curated, not exhaustive)

Global `lv` object exposing roughly:

- `lv.screen()`, `lv.obj/button/label/slider/switch/arc/list(parent, props)`
- props: size, align, text, colors, font size (maps to the 3 compiled montserrat fonts: 14/16/20)
- `widget.on("click"|"change"|"pressing", jsFn)` — C trampoline stores the `JSValue`, LVGL event cb re-enters the VM
- `lv.timer(ms, jsFn)` wrapping `lv_timer_create`
- `console.log` → Serial; `sys.heap()`, `sys.battery()`, `wifi.scan(cb)` (reuse the async-scan + poll pattern from `WaveshareVitals.ino`)

GC discipline: every stored `JSValue` is `JS_DupValue`d and freed when its widget is deleted (hook `LV_EVENT_DELETE`) — this is the main correctness risk of the whole project; design it before writing widget functions.

### Phase 3 — script loading

1. Boot order: try `/sd/app.js` (SD_MMC 4-bit — pins in `board_pins.h`: CLK 16, CMD 15, D0–D3 17/18/13/14; a 32 GB card is already in the slot), else `app.js` on the 9.9 MB FATFS partition, else a built-in fallback script in flash that shows "no app.js found" on the panel.
2. v1 workflow is "edit on PC, move card." Helper scripts can come later.

### Phase 4 — hot reload

- Long-press BOOT (GPIO0; the C demo's loop already shows the debounce pattern) → tear down the JS context, `lv_obj_clean(screen)`, re-read app.js, rebuild. Iteration loop becomes: edit file → card in → press button. No toolchain on the inner loop.
- Stretch: reload over serial, then over WiFi.

### Phase 5 — proof + docs

- Reimplement the C demo's **System tab** (labels, slider, live values) as `app.js` — it exercises props, events, timers, and a native call in ~40 lines of JS.
- Document the binding API and the GC ownership rules in `docs/lang-js/`.

## Risks, honestly

- **QuickJS on Xtensa build friction** (Phase 1 exists to surface this early; JerryScript is the escape hatch).
- **JSValue lifetime bugs** — use-after-free between LVGL widget deletion and JS GC. Mitigated by the DELETE-hook ownership rule and by keeping v1's surface small.
- **Scope creep** — the lvgljs feature list (CSS, JSX, animations, images) is a siren. v1 is ~15 functions; everything else earns its way in.
- **PSRAM latency** — JS heap in PSRAM is slower than SRAM; acceptable because JS runs at event rate, not pixel rate. If eval feels sluggish, keep the engine's hot structures in internal RAM and put only object storage in PSRAM.

## What this is not

Not a lvgljs port, not React, not npm. It's "MicroPython-style scripting, but JavaScript, for this one board" — the 20% of lvgljs that delivers 80% of the point: UI logic as editable data.
