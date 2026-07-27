# System architecture

How the repo is put together. Board-level facts (pinout, chip quirks) are in [`hardware/README.md`](hardware/README.md); build commands are in [`BUILDING.md`](../BUILDING.md).

## The organizing idea

A scriptable firmware platform: the compiled part is flashed once and rarely changes, and the UI is JavaScript the firmware loads at boot from an SD card or a flash partition. Two seams matter, and the directory layout is built around them.

The first is **firmware against apps**. `firmware/` is everything that gets flashed; `app/` is data it reads. Changing an app means saving a file and long-pressing a button, about a second, with no compiler involved.

The second is **platform against board**. Inside `firmware/`, the two Arduino libraries (`quickjs-ng/`, `lvgl-js-bindings/`) contain no pin numbers, no resolution, and no panel name; `boards/<name>/` contains nothing but those. A new board is a pinout, a display bring-up, three host hooks, and a config struct: 189 lines here.

That seam is drawn at hardware, not at "compiled versus scripted". Which file boots, what happens when it throws, the button that gets you out of an app, the serial upload protocol: all of that is compiled C, and none of it depends on the panel, so it sits on the library side in `jsvm_app.cpp`. It was in the sketch until a port attempt had to copy it and the two copies diverged.

```
                        app/  app.js, apps/*.js        data on SD or FATFS
                              ▼
                        QuickJS-ng VM                  firmware/quickjs-ng/
                        binding layer                  firmware/lvgl-js-bindings/
                              ▼
                          LVGL 9.5                     widget tree, layout, render
                              ▼
                   flush cb + Arduino_GFX               one RGB565 byte swap
                              ▼                        firmware/boards/<name>/
                    JD9853 panel over SPI               172×320, rotated to landscape
                    AXS5106L touch over I2C             polled, mapped sx=raw_y
```

The runtime pays roughly 429 KB of flash and ~80 KB of PSRAM for this, and gives up compile-time checking.

Alongside it, [`experiments/c-dashboard/`](../experiments/c-dashboard/README.md) is a complete four-tab LVGL dashboard compiled into a single image with no scripting. It is where the display pipeline and touch driver were first proven against hardware, which is why it is kept. It is frozen: no new features, and no work to bring it to parity with the platform.

## The hardware foundation

Four concerns are solved identically in the board sketch and in the frozen C dashboard, and were solved first in the latter:

- **Display bring-up.** The panel is a JD9853 driven through Arduino_GFX's ST7789 class plus a JD9853-specific register blob, with a 34-pixel column offset centering the 172-wide panel in the controller's 240-wide RAM. Details: [`hardware/display-pipeline.md`](hardware/display-pipeline.md).
- **The flush path.** LVGL renders RGB565 little-endian, the panel wants big-endian, and exactly one swap happens: `lv_draw_sw_rgb565_swap()` followed by `draw16bitBeRGBBitmap()`. Pairing either with the wrong partner double-corrects and produces wrong colors.
- **Draw buffers.** Two partial buffers, 13,440 bytes each, allocated `MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA`. They cannot live in PSRAM because SPI DMA cannot reach it. Their size must be computed from 2 bytes per pixel, never from `sizeof(lv_color_t)`, which is 3 in LVGL 9 and unrelated to the render format.
- **Touch.** The AXS5106L is polled over I2C rather than interrupt-driven, because two sources disagree about which GPIO is RST and which is INT; polling makes the ambiguity harmless. The axis mapping is `sx = raw_y, sy = raw_x`, measured on hardware. Details: [`hardware/touch.md`](hardware/touch.md).

One process-level rule holds everywhere: **a single task does everything.** `loopTask` runs `lv_timer_handler()`, the flush callback, the touch read, and the entire VM. No LVGL locking exists anywhere in this repo because nothing else ever touches LVGL.

## The platform

Two Arduino libraries hold the reusable part: `quickjs-ng/` (the vendored engine) and `lvgl-js-bindings/` (the LVGL bindings plus the app supervisor). `boards/waveshare-s3-touch-147/` is the sketch that owns the hardware, and `app/app.js` is the launcher that ships to the board.

The board sketch is the source of hardware truth. Its glue files (`board_pins.h`, `jd9853_panel.h`, `axs5106l_touch.*`, `lv_conf.h`) started as copies of the C dashboard's, and hardware fixes now land here; the frozen dashboard is not updated to match. One divergence is deliberate rather than drift: the board sketch's `lv_conf.h` additionally enables Montserrat 28 and 40, because scripts can select those sizes and the C dashboard has no need of them.

Everything above the hardware, meaning the VM, the LVGL bindings, the ownership machinery, the boot chain, the reload path, and the serial protocol, is documented in [`runtime-architecture.md`](runtime-architecture.md). What a port to another board actually has to supply is in [`portability.md`](portability.md).

## The frozen C dashboard

`experiments/c-dashboard/app/` is one firmware image containing a four-tab dashboard. Its structure is flat on purpose: four `build*Tab()` functions called once from `buildUi()`, two `lv_timer`s driving live values (vitals at 500 ms, WiFi scan polling at 250 ms), and a `loop()` that calls `lv_timer_handler()`, maintains the FPS counter, and debounces the BOOT button.

Worth knowing: the FPS figure counts flush-callback invocations, not loop iterations, because loop iterations only measure polling rate. The "load" arc is render throughput against a 30 flush/s target, not CPU load, and is labelled as such on screen. Battery voltage reads `null`-ish when WiFi is active, because GPIO12 is an ADC2 channel and the radio arbitrates ADC2.

Which parts of it are board-specific and which transfer to other hardware is analyzed file by file in [`experiments/c-dashboard/portability.md`](experiments/c-dashboard/portability.md).

## Constraints that shape everything

Several decisions across the repo trace back to a handful of hard facts about this board:

The ESP32-S3**R8** has **octal** PSRAM, so `PSRAM=opi` is mandatory and any other setting boot-loops. There are 8 MB of PSRAM but it is slower than internal SRAM, which is why draw buffers stay internal and the JS heap goes to PSRAM: JavaScript runs at event rate, pixels at frame rate. The panel is 172×320 native, used rotated to 320×172, and at 1.47 inches it holds six to eight readable lines, which is why the UIs use tabs rather than dense panes. Serial only works over the native USB port with `CDCOnBoot=cdc`, and only with a data cable.

## Reading order

Someone new to the repo, in order: [`BUILDING.md`](../BUILDING.md) to get something running, [`hardware/README.md`](hardware/README.md) for the board, this document for the shape of things, then [`binding-api.md`](binding-api.md) if you are writing an app, or [`runtime-architecture.md`](runtime-architecture.md) plus [`hardware/display-pipeline.md`](hardware/display-pipeline.md) and [`hardware/touch.md`](hardware/touch.md) if you are changing the firmware.
