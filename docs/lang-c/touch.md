# Touch: the AXS5106L driver and the axis-mapping bug

The most instructive part of this project. Short version: **the display's
rotation math does not apply to the touch overlay**, and assuming it does
mirrors every tap.

## The chip and its protocol

AXS5106L capacitive controller, I2C addr `0x63` (bus: SDA 42 / SCL 41 @ 400 kHz).

| Register | Contents |
|---|---|
| `0x08` | chip ID, 3 bytes — reads `51 06 01` on this unit |
| `0x01` | touch report, 14 bytes |

Touch report layout:

```
data[1]              = number of active points
data[2..3]           = point 0 X: ((data[2] & 0x0F) << 8) | data[3]   (12-bit)
data[4..5]           = point 0 Y: same encoding
(+6 bytes per additional point — unused here)
```

## Why the driver polls instead of using the interrupt

Authoritative sources **disagree** about which of GPIO47/GPIO48 is INT vs RST
(Waveshare's ESP-IDF BSP says INT=47/RST=48; a known-working community sketch
says the reverse). Rather than bet, `touch_begin()`:

1. drives **both** pins low then high (whichever is really RST resets the chip;
   pulsing the other is harmless),
2. releases both to `INPUT_PULLUP` so we never fight the controller's INT
   output,
3. **polls** register `0x01` every UI frame (~200 µs of I2C per frame).

The ambiguity physically cannot break touch this way. Verified: chip answers
with a sane ID and reports touches reliably.

## The axis-mapping bug (found + fixed on hardware, 2026-07-25)

The controller reports in the panel's native 172×320 portrait frame; the UI is
320×172 landscape, so a coordinate transform is needed.

**The wrong reasoning (v1):** the display uses `setRotation(1)` →
`MADCTL = MX | MV` — swap axes, then mirror X. So the touch transform was
written to match: `sx = raw_y; sy = 171 - raw_x`.

**Observed symptoms with v1:**

- Swipes worked (a vertically mirrored swipe is still a horizontal swipe).
- No tap ever activated the tab bar, buttons, or slider.
- Tabs "mysteriously started working" at one point — the user was dragging
  along the *bottom* edge, and the mirrored coordinates were being delivered to
  the tab strip at the *top*.

**How it was diagnosed:** a temporary trace in `touch_read()` printed
`raw=(x,y) mapped=(x,y)` over serial at 10 Hz while the user pressed known
screen locations. Key readings:

| Evidence | Reading | Conclusion |
|---|---|---|
| Taps on the tab bar (top edge) | `raw_x = 5..31` | small `raw_x` = **top** |
| 2-second hold on top-left corner | `raw=(35,41)` | `raw` ≈ (0,0) at top-left |
| Drags along the bottom edge | `raw_x = 143..152` | large `raw_x` = **bottom** |

So `raw_x` **already increases downward** in landscape. The correct transform
is a plain transpose — no mirror:

```cpp
uint16_t sx = raw_y;   // 0..319 left→right
uint16_t sy = raw_x;   // 0..171 top→bottom
```

**Why the reasoning failed:** MADCTL's mirror bit exists to compensate the
*glass's* scan direction — a property of how the display panel is wired to its
controller. The touch overlay is a separate sensor with its own frame, which on
this module happens to already agree with the landscape orientation after a
transpose. Display rotation flags describe the display, nothing else.

**The fix that finally shipped:** one line, `sy = (171) - raw_x` → `sy = raw_x`.
After it: tab taps, the touch-follow dot, the backlight slider, and list
scrolling all confirmed working by hand.

## If you reuse this driver on other hardware

Don't trust any derived mapping — measure. Re-add the trace (this exact block,
which was used for the calibration and then removed):

```cpp
static uint32_t last_dbg = 0;
if (millis() - last_dbg > 100) {
  last_dbg = millis();
  Serial.printf("[touch] raw=(%u,%u) mapped=(%u,%u)\n",
                (unsigned)raw_x, (unsigned)raw_y, (unsigned)sx, (unsigned)sy);
}
```

Then hold each screen corner for ~2 s and read the log. Four corners uniquely
determine the transform (swap? mirror X? mirror Y?) in one session.
