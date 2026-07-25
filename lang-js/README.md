# The JavaScript way 🚧

UI logic for the Waveshare ESP32-S3-Touch-LCD-1.47 written in **JavaScript**,
running on a QuickJS engine embedded in the firmware. Not started yet — the
full phased plan is at
[`docs/lang-js/js-scripting-plan.md`](../docs/lang-js/js-scripting-plan.md).

## The idea in one paragraph

The MCU never runs JS natively; the flashed firmware is still C, but it
contains a small JS VM (QuickJS-ng, heap in the board's 8 MB PSRAM) plus a
hand-written binding layer exposing a curated slice of LVGL
(`lv.button(...)`, `.on("click", fn)`, `lv.timer(...)`). The app itself is an
`app.js` file on the SD card — data, not code — so the edit loop is: edit on
PC → insert card → press BOOT to reload. No compiler, no reflash.

## Why not just use lvgljs?

[lvgljs](https://lvgl.io/docs/open/integration/bindings/javascript) targets
**embedded Linux** (Raspberry Pi-class hardware): its stack includes libuv and
curl, which need POSIX. This board runs FreeRTOS. We keep lvgljs's
architecture (engine + bindings + scripts-as-data) and drop its Linux-only
layers. Details and trade-offs are in the plan.

## Planned layout

```
lang-js/
  JsSpike/        Phase 1: QuickJS-ng vendored + eval-over-serial go/no-go
  <runtime>/      later: the real firmware (engine + LVGL bindings + loader)
  app/            the JavaScript that ships to the SD card
```

Until then, the working reference implementation of everything (display init,
touch, WiFi) is the C way: [`../lang-c/`](../lang-c/README.md).
