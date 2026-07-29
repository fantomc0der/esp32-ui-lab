# esp32-ui-lab

A scriptable LVGL firmware for ESP32 touch panels: flash it once, then build the UI in JavaScript files you edit on a PC and hot-reload on the device without recompiling. The firmware embeds a QuickJS-ng engine and a curated LVGL binding surface; the apps are data it loads at boot.

**Hardware.** Everything here is developed and verified on a **Waveshare ESP32-S3-Touch-LCD-1.47** (ESP32-S3R8, 1.47" IPS 172×320, JD9853 controller, AXS5106L touch), which is the only board it has actually run on. That is a limit of what's been tested, not of the code: the runtime needs an ESP32 with PSRAM and any display LVGL can drive, at any resolution, and reaches hardware through three host hooks. What it does and doesn't require is in [`docs/portability.md`](docs/portability.md); this board's pinout and driver notes are in [`docs/hardware/README.md`](docs/hardware/README.md).

## What's here

### [`firmware/`](firmware/README.md) — the platform

Two Arduino libraries plus one sketch per board. [`quickjs-ng/`](firmware/quickjs-ng/README.md) is the vendored JS engine and [`lvgl-js-bindings/`](firmware/lvgl-js-bindings/README.md) is the LVGL binding layer; neither knows anything about a particular panel. [`boards/waveshare-s3-touch-147/`](firmware/boards/waveshare-s3-touch-147/README.md) is the board-specific half: pinout, display bring-up, and the three host hooks. Build and flash with `.\flash.ps1` from the repo root.

### [`app/`](app/) — the apps

`app.js` is the launcher, which lists `apps/*.js` and runs whichever you tap. Deploy by copying to an SD card and long-pressing BOOT (≥700 ms), or push over serial with `.\push.ps1`. `selftest.js` is the on-hardware test of the binding surface. Script API: [`docs/binding-api.md`](docs/binding-api.md).

Apps can be written two ways. Directly against the bindings, which is what [`docs/binding-api.md`](docs/binding-api.md) documents and what [`app/selftest.js`](app/selftest.js) still does. Or as JSX components with hooks, which is what every shipped app now does: `src/*.jsx` builds to `apps/*.js` with `node tools/build-app.mjs`, using the reconciler in [`app/lib/ui.js`](app/lib/ui.js). That layer is pure JavaScript over the same bindings and adds nothing to the firmware — see [`docs/ui-runtime.md`](docs/ui-runtime.md).

Both remain supported. The pre-port version of each app is frozen in [`tools/fixtures/`](tools/fixtures/) as the baseline a parity test compares against, so the imperative style stays readable side by side with what replaced it.

### [`experiments/c-dashboard/`](experiments/c-dashboard/README.md) — frozen

A complete 4-tab LVGL dashboard (heap chart, touch tester, WiFi scanner, system panel) compiled into a single firmware image, with no scripting. This is where the display pipeline and touch driver were first proven on hardware, which is why it is kept rather than deleted. It is frozen: it gets no new features, and it is not developed toward parity with the JS platform.

## Docs

[`docs/`](docs/README.md) is indexed by topic. Starting points: [`docs/architecture.md`](docs/architecture.md) for the shape of things, [`docs/binding-api.md`](docs/binding-api.md) if you are writing an app, [`docs/runtime-architecture.md`](docs/runtime-architecture.md) if you are changing the runtime, and [`docs/hardware/README.md`](docs/hardware/README.md) for the board.

## Quick start

Build/deploy instructions (toolchain setup, the FQBN, the flash scripts, getting `app.js` onto the board) live in [`BUILDING.md`](BUILDING.md). The short version: run `.\flash.ps1` at the repo root. The board shows up as a `USB Serial Device` on the native USB-C port — use a **data** cable.
