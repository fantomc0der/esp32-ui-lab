# Portability: what the JS runtime actually requires

The counterpart to [`experiments/c-dashboard/portability.md`](experiments/c-dashboard/portability.md), answering "would this run on anything other than this board?" The short answer is yes, on **any ESP32 with PSRAM**, driving **any display LVGL can drive**, at any resolution. What's tied to this board is the sketch, not the runtime.

## The split

| Component | Lines | Board-specific? |
|---|---|---|
| `lvgl-js-bindings/src/` | 2127 | **0%** — knows LVGL, QuickJS and the Arduino ESP32 core. No pins, no panel, no resolution. The `sys` and `wifi` modules compile out via `-DJSVM_WITH_SYS=0` / `-DJSVM_WITH_WIFI=0`. |
| `quickjs-ng/src/` | vendored | **0%** — plain C, five Xtensa type patches (see [engine-notes.md](engine-notes.md)). |
| `boards/<name>/<name>.ino` | 577 | **~35%** — display construction, pin use, SD_MMC wiring, and the three host hooks. The loader, reload, and serial protocol are policy you'd likely keep. |
| `boards/<name>/` hardware headers | — | **100%** — this is the board, by definition; the frozen C dashboard's [portability doc](experiments/c-dashboard/portability.md) breaks the same files down line by line. |
| `app/app.js` and `app/apps/` | 571 | **~85%** — geometry is derived from `lv.size()`, percentages and alignment rather than written for 320×172, so the apps fill a different panel instead of sitting in a corner of it. The residual 15% is the pixel constants that are tuned to font metrics (a header's height, the gap between two lines), and they are load-bearing: a panel small enough to crowd them still needs a look. |

## Hard requirements

**PSRAM, effectively mandatory.** The allocator passes `MALLOC_CAP_SPIRAM` unconditionally, so on a chip without PSRAM the runtime fails to start. Falling back to internal RAM is a one-line change, but the ESP32-S3 has only ~300 KB of internal SRAM with LVGL's draw buffers already competing for it, so a JS heap there would be cramped. Treat PSRAM as required.

This is what excludes the **ESP32-C3 and C6** (no PSRAM at all). It includes the **S3**, the **S2 with PSRAM**, and the classic **WROVER** modules.

**Flash: 4 MB realistically, 8–16 MB comfortably.** The engine alone is ~429 KB, plus LVGL, plus the Arduino core and WiFi stack. This project's firmware is 1.79 MB. The default 4 MB partition scheme gives roughly 1.2 MB of app space, which is not enough, so a custom partition scheme is part of any port.

**LVGL 9.x and the Arduino ESP32 core.** The bindings call LVGL 9 APIs directly (`lv_obj_remove_flag`, `lv_chart_set_axis_range`, and other names that changed from v8), `WiFi.*` for `wifi.scan()`, `ESP.get*` and `getCpuFrequencyMhz()` for `sys.info()`, and `esp_heap_caps.h` for the allocator. That set is the entire platform surface.

## What is *not* a requirement

**Screen size and panel type.** No resolution, pin, or controller name appears anywhere in the binding layer. Your sketch registers whatever display LVGL can drive — SPI, parallel, I2C OLED, e-paper, any resolution — and the bindings build widgets into it.

A caveat on "responsive", since LVGL is often assumed to reflow automatically: it does not. Any resolution *works*, but adapting to one is the script's job. A script that hardcodes pixel coordinates keeps those exact coordinates on a bigger panel, sitting in the top-left corner of it. The bindings give a script three ways to avoid that — percentage sizes (`w: "50%"`), flex layout (`flex: "row"`), and `lv.size()` for the arithmetic those cannot express — and the shipped apps use all three, so they fill a taller or wider panel rather than being tuned to this one. See [Writing for more than one screen](binding-api.md#writing-for-more-than-one-screen).

The one thing that genuinely does not scale is text: fonts are fixed-size bitmaps compiled into the firmware, so a label is the same pixel height on every panel. That makes any dimension chosen to fit text a pixel constant rather than a percentage, which is why the apps mix the two.

**Touch.** The bindings never reference an input device except to read the active pointer's coordinates when one drove an event. A display-only board works; `.on("click", …)` simply never fires.

**Fonts.** The sizes scripts can select are whichever montserrat fonts `lv_conf.h` compiles in, plus the switch in `font_by_size()`. On this board that is five: 14, 16 and 20 unconditionally, and 28 and 40 guarded by `#if LV_FONT_MONTSERRAT_28` / `_40`, so a board sketch whose `lv_conf.h` omits them silently loses those two rather than failing to build. Different sizes are an `lv_conf.h` edit and a few lines.

## Porting scenarios

| Target | Work required |
|---|---|
| Same board, new UI | Edit `app.js`. No firmware change at all. |
| Another ESP32-S3 board with a display | New sketch: LVGL display/indev setup for that panel, plus the three host hooks. Both libraries unchanged, and the shipped apps adapt to the new resolution without editing. Expect an afternoon. |
| ESP32-S2 or WROVER | As above, plus check flash size and partition scheme. `wifi.scan()` and `sys.info()` still work. |
| ESP32-C3 / C6 (no PSRAM) | Not supported as written. Requires reworking the allocator to internal RAM and accepting a small heap; unproven. |
| Non-ESP32 (RP2040, STM32…) | The binding layer's *design* ports, but `esp_heap_caps.h`, `WiFi.h`, and `ESP.*` do not. Expect to replace the allocator and drop or reimplement `wifi.scan()` and `sys.info()`. |

## The porting contract

Everything board-specific reaches the library through three functions, declared in `js_bindings.h` and implemented in the sketch:

```cpp
uint32_t jsvm_host_fps();                  // frames in the last second, or 0
void     jsvm_host_backlight(uint8_t pct); // 0-100, already clamped
float    jsvm_host_battery();              // volts, or NAN for "unavailable"
```

Return `0` and `NAN` for anything a board lacks, and the corresponding script calls degrade to zero and `null` rather than failing. Bring LVGL up however the board requires, implement those three, call `jsvm_start()`, and pump `jsvm_pump()` from `loop()`. That is the whole integration.
