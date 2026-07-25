# The JavaScript way

UI logic for the Waveshare ESP32-S3-Touch-LCD-1.47 written in **JavaScript**, running on a QuickJS-ng engine embedded in the firmware. The full phased plan (and its hardware-verified measurements) is at [`docs/lang-js/js-scripting-plan.md`](../docs/lang-js/js-scripting-plan.md); the script API is at [`docs/lang-js/binding-api.md`](../docs/lang-js/binding-api.md).

## The idea in one paragraph

The MCU never runs JS natively; the flashed firmware is still C, but it contains a small JS VM (QuickJS-ng, heap in the board's 8 MB PSRAM) plus a hand-written binding layer exposing a curated slice of LVGL (`lv.button(...)`, `.on("click", fn)`, `lv.timer(...)`). The app itself is an `app.js` file on the SD card — data, not code — so the edit loop is: edit on PC → insert card → long-press BOOT to reload. No compiler, no reflash.

## Why not just use lvgljs?

[lvgljs](https://lvgl.io/docs/open/integration/bindings/javascript) targets **embedded Linux** (Raspberry Pi-class hardware): its stack includes libuv and curl, which need POSIX. This board runs FreeRTOS. We keep lvgljs's architecture (engine + bindings + scripts-as-data) and drop its Linux-only layers. Details and trade-offs are in the plan.

## Layout

```
lang-js/
  quickjs-ng/     the vendored engine (QuickJS-ng v0.15.1) as an Arduino library
  JsSpike/        Phase 1 spike: engine-on-hardware go/no-go, serial REPL only
  JsHost/         the runtime firmware: display + touch + LVGL + bindings + loader
  app/app.js      the JavaScript app that ships to the SD card
  flash.ps1       build/upload wrapper (adds --library quickjs-ng)
```

The working C reference implementation of the same hardware (display init, touch, WiFi) is [`../lang-c/`](../lang-c/README.md).
