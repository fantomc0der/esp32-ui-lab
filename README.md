# esp32-ui-lab

Touch UIs for ESP32 displays: the same dashboard built twice, once compiled into C firmware and once as JavaScript you edit and hot-reload on the device without recompiling. Each `lang-*/` directory is self-contained, so they can be read, built, and flashed independently.

**Hardware.** Everything here is developed and verified on a **Waveshare ESP32-S3-Touch-LCD-1.47** (ESP32-S3R8, 1.47" IPS 172×320, JD9853 controller, AXS5106L touch), which is the only board it has actually run on. That is a limit of what's been tested, not of the code. The JavaScript runtime is board-agnostic: it needs an ESP32 with PSRAM and any display LVGL can drive, at any resolution, and reaches hardware through three host hooks — what it does and doesn't require is in [`docs/lang-js/portability.md`](docs/lang-js/portability.md). The C demo is board-specific by nature; its pinout and driver notes are in [`docs/hardware.md`](docs/hardware.md).

## What's here

### [`lang-c/`](lang-c/README.md) — C/C++ ✅ working

The classic Arduino approach: everything compiled into one firmware image. A 4-tab LVGL 9 dashboard (heap chart, touch tester, WiFi scanner, system panel), fully verified on hardware, and the reference the JavaScript version is measured against. Start at [`lang-c/README.md`](lang-c/README.md) for setup, board settings, and deployment; `lang-c/flash.ps1` builds and flashes from the terminal.

### [`lang-js/`](lang-js/README.md) — JavaScript ✅ working

UI logic written in JavaScript, executed by a QuickJS-ng engine embedded in the firmware — the idea behind [lvgljs](https://lvgl.io/docs/open/integration/bindings/javascript), re-derived for a FreeRTOS microcontroller (lvgljs itself targets embedded Linux). The **js-host** firmware loads `app.js` from the SD card (or the flash FATFS partition), and a long-press of BOOT hot-reloads it — no recompile, no reflash. Serial doubles as a live JS REPL. Why it exists and what it cost: [`docs/lang-js/design-rationale.md`](docs/lang-js/design-rationale.md); script API: [`docs/lang-js/binding-api.md`](docs/lang-js/binding-api.md).

## Docs

[`docs/`](docs/README.md) mirrors the split:

- [`docs/hardware.md`](docs/hardware.md) — the board itself (pinout, traps, quirks). Applies to both.
- [`docs/lang-c/`](docs/README.md) — display pipeline, touch driver post-mortem, portability analysis, build/flash reference.
- [`docs/lang-js/`](docs/README.md) — the JS runtime: architecture, script API reference, design rationale, build/deploy, and engine notes.

## Quick start

Build/deploy instructions for both (toolchain setup, the FQBN, the flash scripts, getting `app.js` onto the board) live in [`BUILDING.md`](BUILDING.md). The short version: `cd lang-c` (or `lang-js`) and run `.\flash.ps1`. The board shows up as a `USB Serial Device` on the native USB-C port — use a **data** cable.
