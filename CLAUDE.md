# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repo is

A scriptable LVGL firmware platform for ESP32 touch panels, developed on a **Waveshare ESP32-S3-Touch-LCD-1.47** (ESP32-S3R8, 172×320 IPS, JD9853 display controller, AXS5106L touch). `firmware/` is flashed once and holds an embedded QuickJS-ng engine plus a curated LVGL binding layer; `app/` holds the JavaScript the firmware loads at boot, editable and hot-reloadable without recompiling. Two seams organize everything: firmware against apps (`firmware/` vs `app/`), and platform against board (`firmware/{quickjs-ng,lvgl-js-bindings}/` vs `firmware/boards/<name>/`). Full picture: `docs/architecture.md`.

`experiments/c-dashboard/` is a complete compiled-C dashboard, **frozen**: it is where the display and touch stack was first proven, and it is kept for that reason only. Do not propose features for it, and do not develop it toward parity with the platform (the platform has already overtaken it, adding `fetch`, `fs`, text input, and the app/pin model, and lacking nothing the dashboard has).

## Build, flash, and test commands

Everything uses one fixed FQBN — never change `PSRAM=opi` (this board has octal PSRAM; anything else boot-loops) or `CDCOnBoot=cdc` (without it, serial over the native USB-C port is silent):

```
esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=cdc,FlashMode=qio,USBMode=hwcdc
```

One-time toolchain setup:

```powershell
arduino-cli core install esp32:esp32@3.3.11
arduino-cli lib install "lvgl@9.5.0"
arduino-cli lib install "GFX Library for Arduino@1.6.7"
```

Build/flash (`flash.ps1` encodes the FQBN and finds the port):

```powershell
.\flash.ps1                         # the firmware: -BuildOnly, -Port COMx, -Monitor, -Board <name>
cd experiments/c-dashboard; .\flash.ps1   # the frozen C dashboard; same flags
.\push.ps1 app\apps\weather.js      # send one script over serial (never paste by hand)
```

Manual `arduino-cli compile` must link the two vendored libraries explicitly, since they sit in `firmware/` rather than in the Arduino libraries folder:

```powershell
arduino-cli compile --library firmware/quickjs-ng --library firmware/lvgl-js-bindings -b "$FQBN" firmware/boards/waveshare-s3-touch-147
```

What CI (`.github/workflows/ci.yml`) actually checks, without hardware:

```powershell
node --check app/**/*.js                 # syntax-check every script
node tools/check-js-api.mjs              # fail if a script calls a binding the C layer doesn't register
```

What CI *cannot* do: run `app/selftest.js`, the real functional test — it executes on the board and reports over serial, so it needs hardware (deploy it in place of `app.js` and read the serial log, or wire up a self-hosted runner). There is no hosted way to test the UI end-to-end; changes to the binding layer or hardware glue need a real board.

## Architecture

**The hardware stack:** LVGL 9.5 → `Arduino_GFX` flush → JD9853 panel over SPI, AXS5106L touch over I2C. One rule holds everywhere: **a single FreeRTOS task does everything** — `lv_timer_handler()`, the flush callback, touch polling, and the entire JS VM. There is no LVGL locking anywhere because nothing else ever touches LVGL. Two invariants that matter when touching display code:
- The flush path swaps byte order exactly once: `lv_draw_sw_rgb565_swap()` then `draw16bitBeRGBBitmap()`. Pairing either call with the wrong partner double-corrects colors.
- Draw buffer sizing must use 2 bytes/pixel explicitly, never `sizeof(lv_color_t)` (which is 3 in LVGL 9, unrelated to the RGB565 render format). Buffers are `MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA` — SPI DMA cannot reach PSRAM.

**`firmware/`** splits into a reusable half and a board-specific half:
- `quickjs-ng/` — vendored JS engine (v0.15.1), Arduino library, unmodified except for documented Xtensa patches (`docs/engine-notes.md`).
- `lvgl-js-bindings/` — the LVGL binding layer exposed to scripts, an Arduino library with no pin numbers, no resolution, and no panel name in it. Keep it that way; `MALLOC_CAP_SPIRAM` in 4 files is the one real coupling, documented in `docs/portability.md` rather than fixed.
- `boards/<name>/` — one sketch per board: pinout (`board_pins.h`), panel init (`jd9853_panel.h`), touch, `lv_conf.h`, the display stack, and the three host hooks (`jsvm_host_fps()`, `jsvm_host_backlight()`, `jsvm_host_battery()`), plus the script loader/reload path and the serial REPL/upload protocol. **This is the hardware source of truth — fix bugs here.** Its glue files started as copies of `experiments/c-dashboard/app`; that direction is now historical and nothing is copied back. `lv_conf.h` diverges from the dashboard's copy in exactly 2 lines on purpose (Montserrat 28 and 40 enabled, because scripts can select them) — do not "resolve" that by re-copying.

**`app/`** — `app.js` is the launcher shipped to the board; `apps/*.js` are individual apps (`sys.launch(path)` switches between them); `selftest.js` is the on-hardware binding-layer test, deployed in place of `app.js`.

Script boot order: the pinned app if one is set (`sys.pin()` / the `pin` serial command, stored in NVS) → `/app.js` on the SD card → `/app.js` on the flash FATFS partition → a built-in fallback screen. A pin also suppresses the firmware's corner button while the pinned app runs, so BOOT long-press is the only way back to the launcher on a pinned board. Edit loop: edit on PC → reinsert card → long-press BOOT (≥700 ms) to reload, no recompile.

The bottom-right corner is one firmware-owned slot on `lv_layer_top()` holding at most one control, picked in `updateCornerButton()`: **Wi-Fi** → `/apps/wifi.js` when `jsvm_network_setup_needed()`, else **back** → the pinned app, else **home** → the launcher, else nothing when you are already at the target. Wi-Fi ranks first because a pinned board draws nothing there otherwise, leaving no discoverable route to network setup.

`jsvm_network_setup_needed()` is true when the running script used `fetch()`/`wifi.status()`, is not connected, and the failure is one a person can fix: nothing saved, a rejected password, or a network missing for 6 retry attempts (≈64 s). Transient failures stay hidden so the supervisor can retry and the app can just say "offline" — that split lives in `failure_needs_a_person()` in `bindings_wifi.cpp`, keyed on the same reason codes as `reason_name()`, so the two must stay in agreement.

**JS binding surface** (`lv`, `sys`, `fs`, `wifi`, `fetch`, `console` — full reference in `docs/binding-api.md`): 11 widget makers plus ~30 module functions, curated rather than a full binding. Key invariants when changing the binding layer (`firmware/lvgl-js-bindings/src`):
- Callbacks passed to `.on()`/`lv.timer()` are retained with `JS_DupValue` and released exactly once, via an `LV_EVENT_DELETE` hook or `.stop()`.
- Widget handles are weak pointers; using one after its container is `.clean()`'d throws a JS `TypeError` rather than corrupting memory.
- Teardown order on reload is fixed: JS timers → `lv_obj_clean(screen)` → screen-level bindings → JS context → runtime.
- `tools/check-js-api.mjs` derives the binding surface by scanning `JS_SetPropertyStr` calls in `lvgl-js-bindings/src/*.cpp` and the `kMakers[]` table — if you add a binding, it's picked up automatically as long as it goes through `JS_SetPropertyStr`; nothing needs updating in the checker itself.

Full internals (JSValue ownership, call/event flow, memory map): `docs/runtime-architecture.md`. What a port to another board must supply: `docs/portability.md`.

## Board-specific facts worth knowing before changing hardware code

- ESP32-S3**R8** has octal PSRAM — `PSRAM=opi` is non-negotiable, everything else boot-loops.
- Touch axis mapping is a measured plain transpose, `sx = raw_y; sy = raw_x`, with no mirroring — do not "fix" this by reasoning from the display's MADCTL rotation bits (that was the original bug).
- Touch INT/RST pins are ambiguous between Waveshare's BSP and community sketches, so `touch_begin()` pulses both pins and polls over I2C rather than trusting either assignment.
- Battery voltage (`sys.battery()` / GPIO12) reads `null`-ish while WiFi is active — GPIO12 is an ADC2 channel and the radio arbitrates ADC2.
- This board variant has **no addressable RGB LED** (GPIO38 is the LCD clock here, unlike the non-touch 1.47" variant where it's a WS2812).
- Only the native USB-C port with a **data** cable works for flashing/serial; charge-only cables enumerate nothing, with no error.

Deeper board/chip detail: `docs/hardware/README.md`. Per-topic post-mortems: `docs/hardware/touch.md` (axis-mapping bug), `docs/hardware/display-pipeline.md`, `docs/engine-notes.md` (QuickJS heap-poisoning trap, job pump, DTR/RTS bootloader trap).

## Documentation conventions

Prose in `docs/` and `README.md` files is never hard-wrapped: one continuous line per paragraph or list item, letting it soft-wrap in the reader's editor. Line breaks are only for structure (list items, paragraphs, code blocks, table rows). Match this when editing existing docs.
