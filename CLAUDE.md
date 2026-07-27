# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repo is

Touch UIs for a **Waveshare ESP32-S3-Touch-LCD-1.47** (ESP32-S3R8, 172×320 IPS, JD9853 display controller, AXS5106L touch), built two ways for comparison: `lang-c/` compiles the UI into one firmware image, `lang-js/` runs the UI as a JavaScript file (`app.js`) executed by an embedded QuickJS-ng engine, editable and hot-reloadable without recompiling. Both share the same display/touch bring-up, verified first in `lang-c/app`. Full picture: `docs/architecture.md`.

## Build, flash, and test commands

Both sketches use one fixed FQBN — never change `PSRAM=opi` (this board has octal PSRAM; anything else boot-loops) or `CDCOnBoot=cdc` (without it, serial over the native USB-C port is silent):

```
esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=cdc,FlashMode=qio,USBMode=hwcdc
```

One-time toolchain setup:

```powershell
arduino-cli core install esp32:esp32@3.3.11
arduino-cli lib install "lvgl@9.5.0"
arduino-cli lib install "GFX Library for Arduino@1.6.7"
```

Build/flash (each directory has `flash.ps1`, which encodes the FQBN and finds the port):

```powershell
cd lang-c; .\flash.ps1              # C firmware: -BuildOnly, -Port COMx, -Monitor
cd lang-js; .\flash.ps1             # JS runtime firmware (js-host); same flags
```

Manual `arduino-cli compile` for the JS side must link the two vendored libraries explicitly, since they sit beside the sketch rather than in the libraries folder:

```powershell
arduino-cli compile --library lang-js/quickjs-ng --library lang-js/lvgl-js-bindings -b "$FQBN" lang-js/js-host
```

What CI (`.github/workflows/ci.yml`) actually checks, without hardware:

```powershell
node --check lang-js/app/**/*.js         # syntax-check every script
node tools/check-js-api.mjs              # fail if a script calls a binding the C layer doesn't register
```

What CI *cannot* do: run `lang-js/app/selftest.js`, the real functional test — it executes on the board and reports over serial, so it needs hardware (deploy it in place of `app.js` and read the serial log, or wire up a self-hosted runner). There is no hosted way to test either UI end-to-end; changes to the binding layer or hardware glue need a real board.

## Architecture

**The shared stack (identical in both languages):** LVGL 9.5 → `Arduino_GFX` flush → JD9853 panel over SPI, AXS5106L touch over I2C. One rule holds everywhere: **a single FreeRTOS task does everything** — `lv_timer_handler()`, the flush callback, touch polling, and (in `lang-js`) the entire JS VM. There is no LVGL locking anywhere because nothing else ever touches LVGL. Two invariants that matter when touching display code:
- The flush path swaps byte order exactly once: `lv_draw_sw_rgb565_swap()` then `draw16bitBeRGBBitmap()`. Pairing either call with the wrong partner double-corrects colors.
- Draw buffer sizing must use 2 bytes/pixel explicitly, never `sizeof(lv_color_t)` (which is 3 in LVGL 9, unrelated to the RGB565 render format). Buffers are `MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA` — SPI DMA cannot reach PSRAM.

**`lang-c/`** — one firmware image, `lang-c/app/app.ino`. Flat structure: four `build*Tab()` functions called once from `buildUi()`, two `lv_timer`s for live values, `loop()` runs `lv_timer_handler()` plus FPS counting and BOOT-button debounce. This is the hardware source of truth — fix bugs here first.

**`lang-js/`** splits into a reusable half and a board-specific half:
- `quickjs-ng/` — vendored JS engine (v0.15.1), Arduino library, unmodified except for documented Xtensa patches (`docs/lang-js/engine-notes.md`).
- `lvgl-js-bindings/` — the LVGL binding layer exposed to scripts, an Arduino library that knows nothing about this specific board.
- `js-host/` — the firmware: hardware bring-up (identical to `lang-c/app`), the script loader/reload path, and the serial REPL/upload protocol. Its hardware glue files (`board_pins.h`, `jd9853_panel.h`, `axs5106l_touch.*`, `lv_conf.h`) are **verbatim copies from `lang-c/app`** — always fix hardware bugs in `lang-c` first, then re-copy, so there's a single source of truth.
- `app/app.js` — the launcher script shipped to the board; `app/apps/*.js` — individual apps (`sys.launch(path)` switches between them); `app/selftest.js` — the on-hardware binding-layer test, deployed in place of `app.js`.

Script boot order: the pinned app if one is set (`sys.pin()` / the `pin` serial command, stored in NVS) → `/app.js` on the SD card → `/app.js` on the flash FATFS partition → a built-in fallback screen. A pin also suppresses the firmware's corner button while the pinned app runs, so BOOT long-press is the only way back to the launcher on a pinned board. Edit loop: edit on PC → reinsert card → long-press BOOT (≥700 ms) to reload, no recompile.

The bottom-right corner is one firmware-owned slot on `lv_layer_top()` holding at most one control, picked in `updateCornerButton()`: **Wi-Fi** → `/apps/wifi.js` when `jsvm_network_setup_needed()`, else **back** → the pinned app, else **home** → the launcher, else nothing when you are already at the target. Wi-Fi ranks first because a pinned board draws nothing there otherwise, leaving no discoverable route to network setup.

`jsvm_network_setup_needed()` is true when the running script used `fetch()`/`wifi.status()`, is not connected, and the failure is one a person can fix: nothing saved, a rejected password, or a network missing for 6 retry attempts (≈64 s). Transient failures stay hidden so the supervisor can retry and the app can just say "offline" — that split lives in `failure_needs_a_person()` in `bindings_wifi.cpp`, keyed on the same reason codes as `reason_name()`, so the two must stay in agreement.

**JS binding surface** (`lv`, `sys`, `fs`, `wifi`, `fetch`, `console` — full reference in `docs/lang-js/binding-api.md`): about twenty curated LVGL calls, not a full binding. Key invariants when changing the binding layer (`lang-js/lvgl-js-bindings/src`):
- Callbacks passed to `.on()`/`lv.timer()` are retained with `JS_DupValue` and released exactly once, via an `LV_EVENT_DELETE` hook or `.stop()`.
- Widget handles are weak pointers; using one after its container is `.clean()`'d throws a JS `TypeError` rather than corrupting memory.
- Teardown order on reload is fixed: JS timers → `lv_obj_clean(screen)` → screen-level bindings → JS context → runtime.
- `tools/check-js-api.mjs` derives the binding surface by scanning `JS_SetPropertyStr` calls in `lvgl-js-bindings/src/*.cpp` and the `kMakers[]` table — if you add a binding, it's picked up automatically as long as it goes through `JS_SetPropertyStr`; nothing needs updating in the checker itself.

Full internals (JSValue ownership, call/event flow, memory map): `docs/lang-js/architecture.md`.

## Board-specific facts worth knowing before changing hardware code

- ESP32-S3**R8** has octal PSRAM — `PSRAM=opi` is non-negotiable, everything else boot-loops.
- Touch axis mapping is a measured plain transpose, `sx = raw_y; sy = raw_x`, with no mirroring — do not "fix" this by reasoning from the display's MADCTL rotation bits (that was the original bug).
- Touch INT/RST pins are ambiguous between Waveshare's BSP and community sketches, so `touch_begin()` pulses both pins and polls over I2C rather than trusting either assignment.
- Battery voltage (`sys.battery()` / GPIO12) reads `null`-ish while WiFi is active — GPIO12 is an ADC2 channel and the radio arbitrates ADC2.
- This board variant has **no addressable RGB LED** (GPIO38 is the LCD clock here, unlike the non-touch 1.47" variant where it's a WS2812).
- Only the native USB-C port with a **data** cable works for flashing/serial; charge-only cables enumerate nothing, with no error.

Deeper board/chip detail: `docs/hardware.md`. Per-topic post-mortems: `docs/lang-c/touch.md` (axis-mapping bug), `docs/lang-c/display-pipeline.md`, `docs/lang-js/engine-notes.md` (QuickJS heap-poisoning trap, job pump, DTR/RTS bootloader trap).

## Documentation conventions

Prose in `docs/` and `README.md` files is never hard-wrapped: one continuous line per paragraph or list item, letting it soft-wrap in the reader's editor. Line breaks are only for structure (list items, paragraphs, code blocks, table rows). Match this when editing existing docs.
