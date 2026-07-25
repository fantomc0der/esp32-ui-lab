# System architecture

How the repo is put together, and how its two implementations relate. Board-level facts (pinout, chip quirks) are in [`hardware.md`](hardware.md); build commands are in [`BUILDING.md`](../BUILDING.md).

## The organizing idea

One board, two implementations of the same UI, each self-contained in a `lang-*/` directory with its docs mirrored under `docs/lang-*/`. The point is comparison: the same hardware and the same interface, reached by different means, with the trade-offs visible rather than argued.

Both are identical from the panel up to LVGL. They diverge only at the top, in where the UI logic lives and what it costs to change it.

```
              lang-c/                     lang-js/
        ┌──────────────────┐      ┌────────────────────────┐
        │  UI in C, in the │      │  UI in app.js, on the  │
        │  firmware image  │      │  SD card or in FATFS   │
        └──────────────────┘      └───────────┬────────────┘
                 │                            │  QuickJS-ng VM
                 │                            │  binding layer
                 └────────────┬───────────────┘
                              ▼
                          LVGL 9.5              widget tree, layout, render
                              ▼
                   flush cb + Arduino_GFX        one RGB565 byte swap
                              ▼
                    JD9853 panel over SPI        172×320, rotated to landscape
                    AXS5106L touch over I2C      polled, mapped sx=raw_y
```

Change the UI in C and you recompile and reflash, about a minute. Change it in JavaScript and you save a file and long-press a button, about a second. The JavaScript runtime pays roughly 429 KB of flash and ~80 KB of PSRAM for the privilege, and gives up compile-time checking.

## The shared foundation

Four concerns are solved identically in both, and were solved first in `lang-c/WaveshareVitals` against hardware:

- **Display bring-up.** The panel is a JD9853 driven through Arduino_GFX's ST7789 class plus a JD9853-specific register blob, with a 34-pixel column offset centering the 172-wide panel in the controller's 240-wide RAM. Details: [`lang-c/display-pipeline.md`](lang-c/display-pipeline.md).
- **The flush path.** LVGL renders RGB565 little-endian, the panel wants big-endian, and exactly one swap happens: `lv_draw_sw_rgb565_swap()` followed by `draw16bitBeRGBBitmap()`. Pairing either with the wrong partner double-corrects and produces wrong colors.
- **Draw buffers.** Two partial buffers, 13,440 bytes each, allocated `MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA`. They cannot live in PSRAM because SPI DMA cannot reach it. Their size must be computed from 2 bytes per pixel, never from `sizeof(lv_color_t)`, which is 3 in LVGL 9 and unrelated to the render format.
- **Touch.** The AXS5106L is polled over I2C rather than interrupt-driven, because two sources disagree about which GPIO is RST and which is INT; polling makes the ambiguity harmless. The axis mapping is `sx = raw_y, sy = raw_x`, measured on hardware. Details: [`lang-c/touch.md`](lang-c/touch.md).

Both sketches also share one process-level rule: **a single task does everything.** `loopTask` runs `lv_timer_handler()`, the flush callback, the touch read, and in `lang-js/` the entire VM. No LVGL locking exists anywhere in this repo because nothing else ever touches LVGL.

## The C implementation

`lang-c/WaveshareVitals/` is one firmware image containing a four-tab dashboard. Its structure is flat on purpose: four `build*Tab()` functions called once from `buildUi()`, two `lv_timer`s driving live values (vitals at 500 ms, WiFi scan polling at 250 ms), and a `loop()` that calls `lv_timer_handler()`, maintains the FPS counter, and debounces the BOOT button.

Worth knowing: the FPS figure counts flush-callback invocations, not loop iterations, because loop iterations only measure polling rate. The "load" arc is render throughput against a 30 flush/s target, not CPU load, and is labelled as such on screen. Battery voltage reads `null`-ish when WiFi is active, because GPIO12 is an ADC2 channel and the radio arbitrates ADC2.

Which parts of this are board-specific and which transfer to other hardware is analyzed file by file in [`lang-c/portability.md`](lang-c/portability.md).

## The JavaScript implementation

`lang-js/` separates the reusable half from the board-specific half. Two Arduino libraries hold the reusable part: `quickjs-ng/` (the vendored engine) and `lv-binding-js-esp32/` (the LVGL bindings, which know nothing about any particular board). `JsHost/` is the firmware that owns the hardware and the policy decisions, and `app/app.js` is the script that ships to the board.

`JsHost` performs the same hardware bring-up as the C demo, then hands the screen to a JavaScript file. The hardware glue files are **verbatim copies** from `lang-c/WaveshareVitals`: `board_pins.h`, `jd9853_panel.h`, `axs5106l_touch.*`, and `lv_conf.h`. The rule for those is fix in `lang-c` first, then re-copy, so the C demo stays the single source of hardware truth. Duplication was chosen over a shared directory because it keeps each sketch independently openable in the Arduino IDE, which matters for a repo whose purpose is showing complete, self-contained approaches.

Everything above the hardware, meaning the VM, the LVGL bindings, the ownership machinery, the script loader, the reload path, and the serial protocol, is documented in [`lang-js/architecture.md`](lang-js/architecture.md).

## Constraints that shape everything

Several decisions across the repo trace back to a handful of hard facts about this board:

The ESP32-S3**R8** has **octal** PSRAM, so `PSRAM=opi` is mandatory and any other setting boot-loops. There are 8 MB of PSRAM but it is slower than internal SRAM, which is why draw buffers stay internal and the JS heap goes to PSRAM: JavaScript runs at event rate, pixels at frame rate. The panel is 172×320 native, used rotated to 320×172, and at 1.47 inches it holds six to eight readable lines, which is why both demos use tabs rather than dense panes. Serial only works over the native USB port with `CDCOnBoot=cdc`, and only with a data cable.

## Reading order

Someone new to the repo, in order: [`BUILDING.md`](../BUILDING.md) to get something running, [`hardware.md`](hardware.md) for the board, this document for the shape of things, then either [`lang-c/display-pipeline.md`](lang-c/display-pipeline.md) and [`lang-c/touch.md`](lang-c/touch.md) for how the hardware is actually driven, or [`lang-js/architecture.md`](lang-js/architecture.md) and [`lang-js/binding-api.md`](lang-js/binding-api.md) for how JavaScript reaches it.
