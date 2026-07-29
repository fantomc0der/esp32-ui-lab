# Portability: what the JS runtime actually requires

The counterpart to [`experiments/c-dashboard/portability.md`](experiments/c-dashboard/portability.md), answering "would this run on anything other than this board?" The short answer is yes, on **any ESP32 with PSRAM**, driving **any display LVGL can drive**, at any resolution. What's tied to this board is the sketch, not the runtime.

## The split

| Component | Lines | Board-specific? |
|---|---|---|
| `lvgl-js-bindings/src/` | 2621 | **0%** — knows LVGL, QuickJS and the Arduino ESP32 core. No pins, no panel, no resolution. The `sys` and `wifi` modules compile out via `-DJSVM_WITH_SYS=0` / `-DJSVM_WITH_WIFI=0`, and the supervisor (`jsvm_app.cpp`, 442 lines) is optional. |
| `quickjs-ng/src/` | vendored | **0%** — plain C, five Xtensa type patches (see [engine-notes.md](engine-notes.md)). |
| `boards/<name>/<name>.ino` | 189 | **~95%** — display construction, pin use, SD_MMC wiring, the three host hooks, and a config struct. Almost nothing here is reusable, which is the point: everything that was got moved. |
| `boards/<name>/` hardware headers | — | **100%** — this is the board, by definition; the frozen C dashboard's [portability doc](experiments/c-dashboard/portability.md) breaks the same files down line by line. |
| `app/app.js` and `app/apps/` | 571 | **~85%** — geometry is derived from `lv.size()`, percentages and alignment rather than written for 320×172, so the apps fill a different panel instead of sitting in a corner of it. The residual 15% is the pixel constants that are tuned to font metrics (a header's height, the gap between two lines), and they are load-bearing: a panel small enough to crowd them still needs a look. |

## Hard requirements

**PSRAM, effectively mandatory.** `MALLOC_CAP_SPIRAM` is hardcoded in 5 files (`jsvm_core.cpp`, `jsvm_app.cpp`, `bindings_fs.cpp`, `bindings_sys.cpp`, `bindings_wifi.cpp`), so on a chip without PSRAM the runtime fails to start. This is the one real portability coupling in the library, and it is left as a known limitation rather than fixed because every board on the list has PSRAM. Making it a compile-time choice is a small change: one `BOARD_JS_HEAP_CAPS` macro, five call sites.

This excludes the **ESP32-C3 and C6** (no PSRAM at all) as written. It includes the **S3**, the **S2 with PSRAM**, and the classic **WROVER** modules.

**If you do try a PSRAM-less chip, the answer is `MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA`, not `MALLOC_CAP_INTERNAL`.** This was proven on a classic ESP32 (an ESP32-2432S028R), and the failure mode is worth writing down because it looks like the engine is incompatible with the chip: with plain `INTERNAL`, `JS_NewRuntime2()` panics with `LoadStoreError` on its first sub-word write. The cause is that `INTERNAL` alone can return IRAM, which permits only aligned 32-bit access, and QuickJS writes bytes and shorts. Adding `DMA` restricts the allocation to DRAM and it runs. Two numbers from that experiment: the engine costs about 78.8 kB of internal RAM at rest there, and `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)` overstates what is actually usable by roughly 58 kB, because it counts IRAM the allocator cannot hand to a byte-writing caller. Budget against the `INTERNAL|DMA` figure instead.

**Flash: 4 MB realistically, 8–16 MB comfortably.** The engine alone is ~429 KB, plus LVGL, plus the Arduino core and WiFi stack. This project's firmware is 1.94 MB, 64% of its 3 MB app partition. The default 4 MB partition scheme gives roughly 1.2 MB of app space, which is not enough, so a custom partition scheme is part of any port.

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
| Another ESP32-S3 board with a display | New sketch: LVGL display/indev setup for that panel, the three host hooks, and a `JsvmAppConfig`. Both libraries unchanged, no policy to copy, and the shipped apps adapt to the new resolution without editing. Around 190 lines, most of it the display stack. |
| ESP32-S2 or WROVER | As above, plus check flash size and partition scheme. `wifi.scan()` and `sys.info()` still work. |
| ESP32-C3 / C6 (no PSRAM) | Not supported as written, but the engine does run without PSRAM: proven on a classic ESP32 once the heap caps became `INTERNAL\|DMA` (see above). Costs ~79 kB of internal RAM and leaves a heap small enough that script size starts to matter. |
| Non-ESP32 (RP2040, STM32…) | The binding layer's *design* ports, but `esp_heap_caps.h`, `WiFi.h`, and `ESP.*` do not. Expect to replace the allocator and drop or reimplement `wifi.scan()` and `sys.info()`. |

## The porting contract

Everything board-specific reaches the library through three functions, declared in `js_bindings.h` and implemented in the sketch:

```cpp
uint32_t jsvm_host_fps();                  // frames in the last second, or 0
void     jsvm_host_backlight(uint8_t pct); // 0-100, already clamped
float    jsvm_host_battery();              // volts, or NAN for "unavailable"
```

Return `0` and `NAN` for anything a board lacks, and the corresponding script calls degrade to zero and `null` rather than failing.

Everything in the other direction, the parts about *running* scripts, is a struct:

```cpp
JsvmAppConfig cfg;
cfg.sd = &SD_MMC;                 // or null
cfg.flash = &FFat;                // or null
cfg.launcher = "/app.js";
cfg.wifi_app = "/apps/wifi.js";   // null to not offer setup
cfg.home_button_pin = 0;          // active low, long-press opens the launcher; -1 for none
jsvm_app_begin(cfg);              // in setup(), after LVGL and storage
jsvm_app_service();               // in loop(), after lv_timer_handler()
```

One rule about that struct: everything in it is borrowed. `jsvm_app_begin()` copies the struct and keeps it forever, but not what the pointers point at, so the two paths have to be literals or other storage that outlives the call — a `String`'s `c_str()` dangles the moment the `String` does, and the supervisor reads through those pointers on every service pass.

So a whole board sketch is: bring LVGL up however the hardware requires, implement three hooks, fill in that struct, and call two functions. This board's is 189 lines, and roughly 70 of them are the display stack.

The supervisor is opt-in. A product that wants its own boot rules, no corner button, or a different serial protocol skips `jsvm_app_begin()` entirely and drives `jsvm_start()`, `jsvm_stop()` and `jsvm_pump()` itself; that is the interface the supervisor is written against, with no privileged access.

Worth knowing why it exists at all, since it reads like premature generality: it was in the sketch first. A port attempt to a different board copied those 388 lines, both copies then acquired separate bugs, and the port was abandoned partly because of the divergence. Policy that gets copied per board is policy that drifts, so it lives on the library side of the seam.
