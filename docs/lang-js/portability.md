# Portability: what the JS runtime actually requires

The counterpart to [`lang-c/portability.md`](../lang-c/portability.md), answering "would this run on anything other than this board?" The short answer is yes, on **any ESP32 with PSRAM**, driving **any display LVGL can drive**, at any resolution. What's tied to this board is the sketch, not the runtime.

## The split

| Component | Lines | Board-specific? |
|---|---|---|
| `lvgl-js-bindings/src/` | 1190 | **0%** — knows LVGL, QuickJS and the Arduino ESP32 core. No pins, no panel, no resolution. The `sys` and `wifi` modules compile out via `-DJSVM_WITH_SYS=0` / `-DJSVM_WITH_WIFI=0`. |
| `quickjs-ng/src/` | vendored | **0%** — plain C, five Xtensa type patches (see [engine-notes.md](engine-notes.md)). |
| `js-host/js-host.ino` | 332 | **~35%** — display construction, pin use, SD_MMC wiring, and the three host hooks. The loader, reload, and serial protocol are policy you'd likely keep. |
| `js-host/` hardware headers | — | **100%** — verbatim copies from `lang-c/`; see its [portability doc](../lang-c/portability.md). |
| `app/app.js` | 100 | **~60%** — pixel geometry chosen for 320×172. The API calls themselves port unchanged. |

## Hard requirements

**PSRAM, strongly preferred but no longer mandatory.** The allocator's capability flag is now per-target (`JS_HEAP_CAPS`), and the runtime has been shown to start and evaluate scripts from internal RAM on a PSRAM-less classic ESP32 — see [cyd-target.md](cyd-target.md) for the measurements and what it costs.

What it costs is most of your headroom. The VM takes ~66 KB to stand up even with the intrinsic set trimmed, and on that board only ~22 KB was left for scripts after LVGL, WiFi and the SD driver took theirs: enough for a 1.5 KB launcher, not enough for a 4.5 KB app. A board without PSRAM will run small scripts and will not run large ones.

Two hard requirements if you try it. The heap caps must be `MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA`, since bare `MALLOC_CAP_INTERNAL` can return IRAM that QuickJS's byte-level writes fault on. And the draw buffers, LVGL's static pool and the loop-task stack all come from the same pool the JS heap does, so each is a lever and each is also a competitor.

This still excludes the **ESP32-C3 and C6** on flash size and RAM rather than PSRAM alone. It includes the **S3**, the **S2 with PSRAM**, the classic **WROVER** modules, and — with the caveats above — classic ESP32 modules without PSRAM.

**Flash: 4 MB realistically, 8–16 MB comfortably.** The engine alone is ~429 KB, plus LVGL, plus the Arduino core and WiFi stack. This project's firmware is 1.79 MB. The default 4 MB partition scheme gives roughly 1.2 MB of app space, which is not enough, so a custom partition scheme is part of any port.

**LVGL 9.x and the Arduino ESP32 core.** The bindings call LVGL 9 APIs directly (`lv_obj_remove_flag`, `lv_chart_set_axis_range`, and other names that changed from v8), `WiFi.*` for `wifi.scan()`, `ESP.get*` and `getCpuFrequencyMhz()` for `sys.info()`, and `esp_heap_caps.h` for the allocator. That set is the entire platform surface.

## What is *not* a requirement

**Screen size and panel type.** No resolution, pin, or controller name appears anywhere in the binding layer. Your sketch registers whatever display LVGL can drive — SPI, parallel, I2C OLED, e-paper, any resolution — and the bindings build widgets into it.

A caveat on "responsive", since LVGL is often assumed to reflow automatically: it does not. Any resolution *works*, but adapting to one is the script's job. A script that hardcodes pixel coordinates (as `app.js` does, for 320×172) keeps those exact coordinates on a bigger panel. The bindings expose percentage sizes (`w: "50%"`) and flex layout (`flex: "row"`) precisely so a script can be written to adapt; see the note in [binding-api.md](binding-api.md). Fonts never scale, since they are fixed-size bitmaps compiled into the firmware.

**Touch.** The bindings never reference an input device except to read the active pointer's coordinates when one drove an event. A display-only board works; `.on("click", …)` simply never fires.

**Fonts.** The three sizes scripts can select (14, 16, 20) are whichever montserrat fonts `lv_conf.h` compiles in, plus the switch in `font_by_size()`. Different sizes are an `lv_conf.h` edit and a few lines.

## Porting scenarios

| Target | Work required |
|---|---|
| Same board, new UI | Edit `app.js`. No firmware change at all. |
| Another ESP32-S3 board with a display | New sketch: LVGL display/indev setup for that panel, plus the three host hooks. Both libraries unchanged. Expect an afternoon. |
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
