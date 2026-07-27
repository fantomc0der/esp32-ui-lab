# The firmware

Everything that gets flashed to the chip: a JS engine, the LVGL binding layer, and one sketch per board. The UI itself is not here — it lives in [`../app/`](../app/) as data the firmware loads at boot.

How it works internally: [`docs/runtime-architecture.md`](../docs/runtime-architecture.md). What scripts can call: [`docs/binding-api.md`](../docs/binding-api.md). Why it was built this way, with measurements: [`docs/design-rationale.md`](../docs/design-rationale.md).

## The idea in one paragraph

The MCU never runs JS natively; the flashed firmware is still C, but it contains a small JS VM (QuickJS-ng, heap in the board's PSRAM) plus a hand-written binding layer exposing a curated slice of LVGL (`lv.button(...)`, `.on("click", fn)`, `lv.timer(...)`). The app itself is an `app.js` file on the SD card — data, not code — so the edit loop is: edit on PC → insert card → long-press BOOT to reload. No compiler, no reflash.

## Why not just use lvgljs?

[lvgljs](https://lvgl.io/docs/open/integration/bindings/javascript) targets **embedded Linux** (Raspberry Pi-class hardware): its stack includes libuv and curl, which need POSIX. This board runs FreeRTOS. We keep lvgljs's architecture (engine + bindings + scripts-as-data) and drop its Linux-only layers. Details and trade-offs are in the [design rationale](../docs/design-rationale.md).

## Layout

```
firmware/
  quickjs-ng/                     the vendored JS engine (v0.15.1), an Arduino library
  lvgl-js-bindings/               the bindings plus the app supervisor — board-agnostic
  boards/
    waveshare-s3-touch-147/       one sketch per board: pinout, display bring-up, 3 hooks
```

The two libraries are the reusable half and contain no pin numbers, no resolution, and no panel name. Everything board-specific is in `boards/<name>/`, which reaches the bindings through three host hooks (`jsvm_host_fps()`, `jsvm_host_backlight()`, `jsvm_host_battery()`) and a `JsvmAppConfig` naming the launcher, the Wi-Fi app, and the button pin. That split is what makes the runtime portable to other ESP32 boards; what a port actually has to supply is spelled out in [`docs/portability.md`](../docs/portability.md).

The seam is drawn at hardware rather than at "compiled versus scripted". Which file boots, what happens when it throws, the corner button, the long-press, the serial upload protocol: all compiled C, none of it dependent on the panel, so it sits in the library as `jsvm_app.cpp` instead of in each sketch. It was in the sketch until a port attempt copied it and the two copies drifted. The supervisor is opt-in: a sketch with its own boot rules drives `jsvm_start()`/`jsvm_stop()`/`jsvm_pump()` directly and never calls `jsvm_app_begin()`. That is why this board's sketch is 189 lines.

Build from the repo root with `.\flash.ps1`, which links both libraries with `--library` since they are not in the Arduino libraries folder. `-Board <name>` picks a different sketch under `boards/`.

(The Phase 1 engine spike, `JsSpike/`, was deleted once the real firmware superseded it; its measurements live on in [`docs/engine-notes.md`](../docs/engine-notes.md) and the sketch itself at commit `34e0a13`.)
