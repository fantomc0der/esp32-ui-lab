# waveshare-esp32

Projects for the **Waveshare ESP32-S3-Touch-LCD-1.47** (ESP32-S3R8, 1.47" IPS 172×320, JD9853 display, AXS5106L touch), explored per-language — each `lang-*/` directory is a self-contained way of programming the board.

## The ways

### [`lang-c/`](lang-c/README.md) — the C/C++ way ✅ working

The classic Arduino approach: everything compiled into one firmware image. Contains **WaveshareVitals**, a 4-tab LVGL 9 demo dashboard (heap chart, touch tester, WiFi scanner, system panel) — fully verified on hardware. Start at [`lang-c/README.md`](lang-c/README.md) for setup, board settings, and deployment; `lang-c/flash.ps1` builds and flashes from the terminal.

### [`lang-js/`](lang-js/README.md) — the JavaScript way 🚧 planned

UI logic written in JavaScript, executed by a QuickJS engine embedded in the firmware — the idea behind [lvgljs](https://lvgl.io/docs/open/integration/bindings/javascript), re-derived for a FreeRTOS microcontroller (lvgljs itself targets embedded Linux). Edit `app.js` on your PC, load it from the SD card, no recompile. The plan lives at [`docs/lang-js/js-scripting-plan.md`](docs/lang-js/js-scripting-plan.md).

## Docs

[`docs/`](docs/README.md) mirrors the split:

- [`docs/hardware.md`](docs/hardware.md) — the board itself (pinout, traps, quirks). Shared: applies to every way.
- [`docs/lang-c/`](docs/README.md) — display pipeline, touch driver post-mortem, portability analysis, build/flash reference.
- [`docs/lang-js/`](docs/README.md) — the JS-runtime plan (and, later, the binding API reference).

## Quick start

You want the working demo: read [`lang-c/README.md`](lang-c/README.md), copy `lang-c/WaveshareVitals/` into your Arduino sketchbook (or run `lang-c/flash.ps1` with arduino-cli), and flash. The board shows up as a `USB Serial Device` on the native USB-C port — use a **data** cable.
