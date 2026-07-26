# Portability: what's device-specific vs. reusable

"How much of this code is specific to this device?" — answered file by file. Total project: ~1 900 lines, but most of that is `lv_conf.h` boilerplate. Of the ~710 lines actually written for this project, roughly **40% is device-specific, 25% is screen-size-specific styling, and 35% is portable logic/patterns.**

## File by file

| File | Lines | Device-specific? |
|---|---|---|
| `board_pins.h` | 69 | **100%** — this exact board's pinout. Any other board: rewrite entirely. |
| `jd9853_panel.h` | 99 | **100%** — JD9853 register blob. Any other panel controller: delete; a supported controller needs no blob at all. |
| `axs5106l_touch.{h,cpp}` | 103 | **~80%** — the I2C protocol is AXS5106L-specific; the axis transform is *this panel's* measured orientation. The polling *pattern* (pulse-both-reset, poll instead of INT) is a reusable idea. |
| `lv_conf.h` | 1 191 | **~1%** — generic LVGL 9 config; only the enabled font sizes (14/16/20) were chosen for this screen's DPI. Reusable nearly as-is. |
| `app.ino` | 441 | **mixed** — see breakdown below. |

## Inside the .ino (441 lines)

**Device-specific (~90 lines):**
- Display construction: `Arduino_ESP32SPI` bus on these pins, `Arduino_ST7789` with 172×320 + the 34-px column offset, `jd9853_init()` call, 40 MHz SPI, manual RST pulse.
- Backlight PWM on GPIO46, battery read on GPIO12 with the ÷2 divider and the ADC2-vs-WiFi caveat, BOOT button on GPIO0.
- Draw-buffer sizing derived from 320×172.

**Screen-size styling (~180 lines):** everything inside the four `build*Tab()` functions uses hardcoded pixel geometry chosen for 320×172 — panel sizes (186×132, 118×132), the 300×104 touch box, the 306×100 WiFi list, 280-px slider, 30-px tab bar, montserrat 14/16 text. The *widgets* are stock LVGL; on a different resolution the layout constants would need retuning (or replacing with percentage/flex sizing — a worthwhile refactor if you ever target two screens).

**Portable (~170 lines):** the LVGL glue and app logic transfer to any ESP32 + Arduino_GFX + LVGL 9 project unchanged in shape:
- flush callback (`rgb565_swap` + `draw16bitBeRGBBitmap`), tick callback, pointer indev callback
- `LV_DISPLAY_RENDER_MODE_PARTIAL` + double buffer in DMA-capable RAM
- `lv_timer_handler()` sleep-hint loop
- LVGL timers driving data updates independent of frame rate
- async WiFi scan + poll-for-completion pattern
- FPS accounting via flush counting

## Porting scenarios, concretely

| Target | Work required |
|---|---|
| Same board, new app | Keep everything except the `build*Tab()` UI; write your own screens. |
| Other ESP32-S3 + ST7789-family SPI panel | New `board_pins.h`; drop or replace the JD9853 blob; adjust W/H/offsets; re-measure touch mapping ([touch.md](touch.md)); retune layout constants. |
| Other touch chip (CST816, GT911, FT6x36…) | Replace `axs5106l_touch.cpp` protocol part; keep the poll-and-transform structure. |
| Non-ESP32 MCU | The LVGL patterns survive; `heap_caps_malloc`, `WiFi`, `ESP.get*`, `analogReadMilliVolts`, LEDC PWM do not. |

## The two numbers to remember

- **172 × 320 portrait native, used as 320 × 172 landscape** — flows through buffer sizing, layout constants, touch clamping.
- **Column offset 34** — the JD9853 quirk of centring 172 px in 240-px RAM. Both live in `board_pins.h` and everything derives from there.
