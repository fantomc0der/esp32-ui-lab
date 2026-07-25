# waveshare-esp32

Two implementations of the same touch UI for the **Waveshare ESP32-S3-Touch-LCD-1.47** (ESP32-S3R8, 1.47" IPS 172×320, JD9853 display, AXS5106L touch), one in C and one in JavaScript. Each `lang-*/` directory is self-contained, so they can be read, built, and flashed independently.

## What's here

### [`lang-c/`](lang-c/README.md) — C/C++ ✅ working

The classic Arduino approach: everything compiled into one firmware image. Contains **WaveshareVitals**, a 4-tab LVGL 9 demo dashboard (heap chart, touch tester, WiFi scanner, system panel) — fully verified on hardware. Start at [`lang-c/README.md`](lang-c/README.md) for setup, board settings, and deployment; `lang-c/flash.ps1` builds and flashes from the terminal.

### [`lang-js/`](lang-js/README.md) — JavaScript ✅ working

UI logic written in JavaScript, executed by a QuickJS-ng engine embedded in the firmware — the idea behind [lvgljs](https://lvgl.io/docs/open/integration/bindings/javascript), re-derived for a FreeRTOS microcontroller (lvgljs itself targets embedded Linux). The **JsHost** firmware loads `app.js` from the SD card (or the flash FATFS partition), and a long-press of BOOT hot-reloads it — no recompile, no reflash. Serial doubles as a live JS REPL. Why it exists and what it cost: [`docs/lang-js/design-rationale.md`](docs/lang-js/design-rationale.md); script API: [`docs/lang-js/binding-api.md`](docs/lang-js/binding-api.md).

## Docs

[`docs/`](docs/README.md) mirrors the split:

- [`docs/hardware.md`](docs/hardware.md) — the board itself (pinout, traps, quirks). Applies to both.
- [`docs/lang-c/`](docs/README.md) — display pipeline, touch driver post-mortem, portability analysis, build/flash reference.
- [`docs/lang-js/`](docs/README.md) — the JS runtime: architecture, script API reference, design rationale, build/deploy, and engine notes.

## Quick start

Build/deploy instructions for both (toolchain setup, the FQBN, the flash scripts, getting `app.js` onto the board) live in [`BUILDING.md`](BUILDING.md). The short version: `cd lang-c` (or `lang-js`) and run `.\flash.ps1`. The board shows up as a `USB Serial Device` on the native USB-C port — use a **data** cable.
