# Display pipeline: LVGL → JD9853

How a pixel gets from an LVGL widget to the glass, and why each stage is the
way it is. All verified on hardware 2026-07-25 (colors, offsets, inversion and
rotation were all correct on first boot).

## The JD9853 problem

Arduino_GFX (1.6.7) has **no JD9853 driver class**. Waveshare's own Arduino
demo solves this by driving it as an `Arduino_ST7789` — the command set is
close enough — and then sending a JD9853-specific register init blob after
`begin()`. We do the same:

```
hard-reset RST (GPIO40)          // manual; Arduino_GFX told GFX_NOT_DEFINED
gfx->begin(40 MHz SPI)           // generic ST7789 bring-up
jd9853_init(bus)                 // vendor power/gamma/timing blob — jd9853_panel.h
gfx->setRotation(1)              // landscape
```

Two JD9853 specifics inside that blob (`jd9853_panel.h`):

- **`0x21` INVON** — this panel needs inversion ON or colors come out negative.
- **CASET `0x22..0xCD`** — the 172-px-wide glass is centred in the
  controller's 240-px-wide RAM, hence `LCD_COL_OFFSET 34` everywhere.

The blob was transcribed **verbatim** from working vendor/community code and
all 13 `WRITE_BYTES` declared lengths machine-audited. Don't hand-tune panel
timing registers — a wrong value is typically a white screen with no error.

## Rotation

The panel is natively 172×320 portrait. The UI runs landscape (320×172) via
`setRotation(1)`, which sets `MADCTL = MX | MV` (`Arduino_ST7789.cpp`):
`MV` exchanges rows/columns, `MX` mirrors the resulting X. Important subtlety:
these bits compensate the *glass's* scan direction — they say nothing about the
touch overlay's coordinate frame (see [touch.md](touch.md) for the bug this
caused).

## RGB565 byte order — exactly one swap

LVGL renders RGB565 **little-endian**; the panel wants **big-endian** on the
SPI wire. LVGL 9 removed `LV_COLOR_16_SWAP`, so the flush callback does:

```cpp
lv_draw_sw_rgb565_swap(px_map, w * h);         // arg is PIXELS, not bytes
gfx->draw16bitBeRGBBitmap(x, y, buf, w, h);    // writes bytes through, NO internal swap
```

The pairing is the invariant: `draw16bitBeRGBBitmap` + explicit swap = one
swap. Using plain `draw16bitRGBBitmap` alongside the swap would double-correct
and give garbled-but-shaped colors.

## Draw buffers

- **Partial render mode**, two buffers, each 1/8 of the screen
  (320 × 21 rows × 2 B = 13 440 bytes).
- Allocated with `heap_caps_malloc(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)` —
  the flush path feeds SPI DMA, and **DMA cannot reach PSRAM**.
- **`sizeof(lv_color_t)` is 3 in LVGL 9 and must never size these buffers.**
  It's a 24-bit r/g/b struct unrelated to the render format. RGB565 = 2
  bytes/px, computed explicitly (`kBytesPerPx = 2`).
  `lv_display_set_buffers()` takes a size in BYTES and derives buffer height
  from it, so the wrong constant silently mis-states geometry.

## Loop cadence

`loop()` sleeps for whatever `lv_timer_handler()` returns (capped at 16 ms)
instead of a fixed delay — no spinning while idle, no starving LVGL while
busy, and the WiFi/IDLE tasks get their time.

## Observed performance

40 MHz SPI, 240 MHz CPU: the demo sustains its UI comfortably; the heap chart
(continuous redraw) is the heaviest widget. The System tab shows a live FPS
counter — that number counts *actual flushes*, not loop iterations.
