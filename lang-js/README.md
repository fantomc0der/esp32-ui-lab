# JavaScript-scripted UI

UI logic for the Waveshare ESP32-S3-Touch-LCD-1.47 written in **JavaScript**, running on a QuickJS-ng engine embedded in the firmware. How it works internally: [`docs/lang-js/architecture.md`](../docs/lang-js/architecture.md). What scripts can call: [`docs/lang-js/binding-api.md`](../docs/lang-js/binding-api.md). Why it was built this way, with measurements: [`docs/lang-js/design-rationale.md`](../docs/lang-js/design-rationale.md).

## The idea in one paragraph

The MCU never runs JS natively; the flashed firmware is still C, but it contains a small JS VM (QuickJS-ng, heap in the board's 8 MB PSRAM) plus a hand-written binding layer exposing a curated slice of LVGL (`lv.button(...)`, `.on("click", fn)`, `lv.timer(...)`). The app itself is an `app.js` file on the SD card — data, not code — so the edit loop is: edit on PC → insert card → long-press BOOT to reload. No compiler, no reflash.

## Why not just use lvgljs?

[lvgljs](https://lvgl.io/docs/open/integration/bindings/javascript) targets **embedded Linux** (Raspberry Pi-class hardware): its stack includes libuv and curl, which need POSIX. This board runs FreeRTOS. We keep lvgljs's architecture (engine + bindings + scripts-as-data) and drop its Linux-only layers. Details and trade-offs are in the [design rationale](../docs/lang-js/design-rationale.md).

## Layout

```
lang-js/
  quickjs-ng/           the vendored JS engine (v0.15.1), an Arduino library
  lvgl-js-bindings/  the LVGL bindings, an Arduino library — board-agnostic
  js-host/               the firmware: hardware bring-up, script loader, reload
  app/app.js            the JavaScript app that ships to the SD card
  app/selftest.js       deploy instead of app.js to check the binding layer
  flash.ps1             build/upload wrapper (links both libraries)
```

The two libraries are the reusable half and know nothing about this board; everything board-specific is in `js-host/`, which reaches the bindings through three host hooks. That split is what makes the JS runtime portable to other ESP32 boards.

(The Phase 1 engine spike, `JsSpike/`, was deleted once js-host superseded it; its measurements live on in [`docs/lang-js/engine-notes.md`](../docs/lang-js/engine-notes.md) and the sketch itself at commit `34e0a13`.)

The working C reference implementation of the same hardware (display init, touch, WiFi) is [`../lang-c/`](../lang-c/README.md).
