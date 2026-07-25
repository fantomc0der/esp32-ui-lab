# WaveshareVitals

An LVGL 9 touch demo for the **Waveshare ESP32-S3-Touch-LCD-1.47** (172×320 IPS, JD9853 display, AXS5106L touch), built for Arduino IDE.

Four swipeable tabs, each chosen to prove a different part of the board works:

| Tab | What it proves |
|---|---|
| **Vitals** | Live free-heap line chart, arc gauge, uptime + battery volts. Continuous redraw — the stress case for a small SPI panel. |
| **Touch** | Drag your finger in the box; a dot follows and coordinates print live. Fastest way to spot a swapped or mirrored axis. |
| **WiFi** | Async AP scan into a scrollable list. Proves the radio works *and* that the UI stays responsive while the driver is busy. |
| **System** | Chip model/revision/cores, flash + PSRAM size, LVGL version, live FPS, and a backlight brightness slider (proves the BL pin + PWM). |

The BOOT button cycles backlight through 5 levels — a liveness check that works even if touch is misbehaving.

Deeper topic notes live in [`docs/`](../docs/README.md): hardware quirks, the display pipeline, the touch axis-mapping story, what's portable vs. device-specific, and build/flash reference.

**Status:** verified on hardware (2026-07-25) — display, colors, touch (taps, drags, slider), WiFi scan, and tab navigation all confirmed working. 40% flash / 29% RAM.

---

## 1. One-time setup

### 1a. Install the ESP32 board support package

1. Open Arduino IDE.
2. **Tools → Board → Boards Manager…** (or the board icon in the left sidebar).
3. Search `esp32`. Install **"esp32" by Espressif Systems** — version **3.0.0 or newer** (this project is verified against 3.3.11).

No custom Boards Manager URL is needed anymore; the Espressif package is in the default index.

> First install is a ~250 MB download and the first compile takes several minutes. That's normal, not a hang.

### 1b. Install the two libraries

**Tools → Manage Libraries…**, then install:

| Search for | Install | Version used here |
|---|---|---|
| `lvgl` | **lvgl** by kisvegabor | 9.5.0 |
| `GFX Library for Arduino` | **GFX Library for Arduino** by Moon On Our Nation | 1.6.7 |

Nothing else — no `TFT_eSPI`, no separate touch library. The AXS5106L driver ships in this sketch folder because it isn't in Library Manager.

### 1c. Put the sketch where Arduino IDE expects it

Arduino requires a sketch's folder name to match its `.ino` name. Copy the whole `WaveshareVitals` folder into your sketchbook:

```
C:\Users\<you>\Documents\Arduino\WaveshareVitals\
    WaveshareVitals.ino
    axs5106l_touch.cpp
    axs5106l_touch.h
    board_pins.h
    jd9853_panel.h
    lv_conf.h
```

Then **File → Open…** and pick `WaveshareVitals.ino`. The other files appear as tabs and compile automatically — you don't add them anywhere.

> **On `lv_conf.h`:** most LVGL-on-Arduino guides tell you to copy `lv_conf.h` into `Documents\Arduino\libraries\` next to the `lvgl` folder. **Don't** — this project ships its own `lv_conf.h` inside the sketch folder, which LVGL 9 picks up via `__has_include`. Verified working. Keeping it here means the config travels with the sketch and survives LVGL upgrades. If you already have a stray `lv_conf.h` in your `libraries\` root from another project, it will win and may break this build — rename it.

---

## 2. Board settings — these matter

**Tools → Board → esp32 → ESP32S3 Dev Module**, then set:

| Setting | Value | Why |
|---|---|---|
| **PSRAM** | **OPI PSRAM** | ⚠️ The single most important one. This board's ESP32-S3**R8** uses *octal* PSRAM. Leaving this on "QSPI PSRAM" or "Disabled" gives a boot loop or no PSRAM. |
| **Flash Size** | **16MB (128Mb)** | Board has 16 MB. |
| **Partition Scheme** | **16M Flash (3MB APP/9.9MB FATFS)** | Default 4 MB scheme only gives 1.2 MB app space; this sketch needs ~1.2 MB and would be a tight/failing fit. |
| **USB CDC On Boot** | **Enabled** | Required to see `Serial.print` over the native USB-C port. Without it the Serial Monitor stays silent. |
| **Flash Mode** | QIO 80MHz | |
| **CPU Frequency** | 240MHz (WiFi) | |
| **Upload Speed** | 921600 | Drop to 115200 if uploads fail. |
| **Core Debug Level** | None | Set to "Info"/"Verbose" when debugging. |

Leave everything else at its default.

> There is no "ESP32-S3-Touch-LCD-1.47" entry in the board list. The core ships a `Waveshare ESP32-S3-LCD-1.47` board — that's the **non-touch** variant with a *different pinout* (its GPIO38 is a WS2812 LED; here GPIO38 is the LCD clock). Use **ESP32S3 Dev Module** with the settings above; this sketch defines its own pins.

---

### Shortcut: build + flash from the terminal

If you'd rather not click through menus, `flash.ps1` encodes every board option above:

```powershell
.\flash.ps1              # auto-detect port, build, upload, then open the monitor
.\flash.ps1 -BuildOnly   # compile only (verified working)
.\flash.ps1 -Port COM7   # force a port
.\flash.ps1 -Monitor     # just open the serial monitor
```

It finds `arduino-cli` on your PATH, or falls back to the copy bundled inside Arduino IDE. The IDE route below works identically — use whichever you prefer.

---

## 3. Upload

1. Plug the board into a USB-C port. Use a **data** cable — charge-only cables are the #1 cause of "no COM port".
2. **Tools → Port** → pick the new `COM*` that appears.
3. Click **→** (Upload).

### If no COM port appears, or upload fails

Manually enter download mode:

1. Hold **BOOT**.
2. Tap **RESET** (or plug in USB while still holding BOOT).
3. Release **BOOT**.
4. Re-check **Tools → Port** — the port number often *changes* in bootloader mode. Select it and upload again.

After a successful upload in this mode, press **RESET** once to run your sketch.

Other things worth knowing:

- **The port number changes between normal and bootloader mode.** That's expected, not a fault — re-select it.
- **Windows 11 needs no driver** for the native USB-C port (it's standard USB CDC, VID `303A`). If you see a CH340/CP2102 port instead, that's a *different* adapter — this board uses the ESP32-S3's built-in USB.
- **`A fatal error occurred: Failed to connect to ESP32-S3`** → use the BOOT/RESET sequence above.
- **Board resets repeatedly / port flickers** → same fix; blank flash can cause this. Once flashed with real firmware it settles.
- **Verify it's really absent vs. a stale entry:** in Device Manager a "phantom" port is remembered, not present. In PowerShell, `[System.IO.Ports.SerialPort]::GetPortNames()` lists only *live* ports.

### Confirming success

Upload output ends with:

```
Hash of data verified.
Leaving...
Hard resetting via RTS pin...
```

Open **Tools → Serial Monitor** at **115200 baud**. You should see:

```
[boot] WaveshareVitals starting
[boot] display 320x172
[touch] AXS5106L ok, id = ...
[boot] lvgl draw buffers: 13760 bytes each
[boot] ready
```

You may need to reopen the Serial Monitor after a reset — native USB re-enumerates and the monitor loses the handle.

---

## 4. Troubleshooting the display

| Symptom | Cause & fix |
|---|---|
| **Backlight on, screen black/white** | Panel init didn't take. Confirm `jd9853_init(bus)` runs *after* `gfx->begin()`. |
| **Colors inverted** (blues look orange) | The JD9853 needs inversion **on**. `jd9853_panel.h` sends `0x21` (INVON) — if you removed it, put it back. |
| **Wrong/garbled colors, correct shapes** | RGB565 byte order. The flush path calls `lv_draw_sw_rgb565_swap()` then `draw16bitBeRGBBitmap()` — both halves are required; using `draw16bitRGBBitmap()` with the swap double-corrects. |
| **Image shifted sideways, edge wraps** | Column offset. 172 px centred in 240 px RAM needs `LCD_COL_OFFSET 34`. |
| **Touch does nothing; serial says "did NOT respond"** | I2C. Check `TOUCH_PIN_SDA 42` / `TOUCH_PIN_SCL 41`. The UI still swipes via tab bar without touch. |
| **Touch axes swapped/mirrored** | The verified mapping is a plain transpose: `sx = raw_y; sy = raw_x` in `touch_read()` (`axs5106l_touch.cpp`) — no mirroring. If yours differs, the Touch tab shows live coordinates to check against. |
| **Boot loop / "Guru Meditation" at startup** | Almost always **PSRAM set to QSPI instead of OPI**. |
| **Low FPS** | Expected range is ~25–40 fps at 40 MHz SPI. Chart redraw is the expensive part. |

---

## Known unknowns

Being straight about what's verified and what isn't:

- ✅ **Verified on hardware (2026-07-25):** boots clean over native USB (COM port, `VID_303A`), display init correct (colors, offsets, inversion all right on first boot), AXS5106L answers on I2C (`id = 51 06 01`), tab taps / touch-follow dot / backlight slider / WiFi scan list all confirmed working by hand. Compiles clean with `--warnings all --clean` against esp32 core 3.3.11 + LVGL 9.5.0 + Arduino_GFX 1.6.7 (40% flash, 29% RAM). Sketch-local `lv_conf.h` confirmed picked up.
- ✅ **The touch axis mapping is now measured, not reasoned.** The touch frame is a plain transpose of the landscape screen: `sx = raw_y; sy = raw_x` — **no mirror**. The original build inverted Y (`171 - raw_x`, derived from the display's MADCTL rotation bits) and every tap landed vertically flipped: swipes still worked, but taps never hit the tab bar. Serial corner-logging pinned it down. Lesson: the display's MADCTL mirror compensates the *panel's* scan direction, which the *touch* coordinate frame never had.
- ⚠️ **Touch INT/RST pins are ambiguous.** Waveshare's ESP-IDF BSP says `INT=47, RST=48`; a known-working community Arduino sketch uses them the other way round. Rather than bet, `touch_begin()` pulses *both* pins to reset the controller, then releases both and **polls** over I2C — so touch works regardless of which is which. Costs ~200 µs per frame.
- ⚠️ **Battery voltage is uncalibrated.** `BAT_PIN 12` and a ÷2 divider are inferred from community code, not a schematic. Treat the reading as indicative. (The community sketch used a ×3.0 fudge factor; this uses the electrically-correct ×2.0, which may read differently.)
- ⚠️ **"Load %" on the Vitals tab is a UI-activity proxy**, derived from frame rate — not a real CPU load counter. Labeled as such in the code.
- ℹ️ **This board has no addressable RGB LED.** The non-touch 1.47" variant does (GPIO38), but here GPIO38 is the LCD clock. Don't drive it as a LED.

## Pin reference

Cross-checked across Waveshare's wiki, their ESP-IDF BSP headers, and a working community sketch — see `board_pins.h` for per-pin source citations.

| Function | GPIO |
|---|---|
| LCD SCK / MOSI / DC / CS / RST / Backlight | 38 / 39 / 45 / 21 / 40 / 46 |
| Touch I2C SDA / SCL | 42 / 41 |
| Touch RST + INT (ambiguous, see above) | 47, 48 |
| microSD CLK / CMD / D0–D3 | 16 / 15 / 17, 18, 13, 14 |
| Battery ADC | 12 |
| BOOT button | 0 |

Touch controller I2C address: `0x63`.
