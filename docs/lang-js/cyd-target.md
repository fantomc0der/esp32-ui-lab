# The CYD target: running the JS host on a board with no PSRAM

Status as of the last hardware session: **incomplete.** The firmware builds and boots on the ESP32-2432S028R, brings up the display, mounts the card, starts the JS VM, and runs `app.js` — but **touch does not work**, so nothing on screen can be tapped and the board cannot actually run an app. Treat this as a documented work-in-progress, not a supported target.

This file exists so the next attempt starts from the measurements rather than re-deriving them. Everything below was measured on hardware unless it says otherwise.

## The board

Sold as the "Cheap Yellow Display" (CYD). It is a *different chip family* from the Waveshare board this project was built for, not a variant of it.

| | Waveshare ESP32-S3-Touch-LCD-1.47 | ESP32-2432S028R |
|---|---|---|
| MCU | ESP32-S3**R8** (Xtensa LX7) | ESP32-D0WD-V3 rev 3.1 (classic, Xtensa LX6) |
| PSRAM | 8 MB octal | **none** |
| Flash | 16 MB | 4 MB |
| Display | JD9853 172×320, SPI | ILI9341 240×320, SPI |
| Touch | AXS5106L, I2C, capacitive | XPT2046, SPI, **resistive** |
| Script storage | 9.9 MB FATFS partition + SD | SD only (4 MB leaves no room) |
| Serial | native USB-C | CH340 UART bridge |

FQBN: `esp32:esp32:esp32:PSRAM=disabled,FlashSize=4M,PartitionScheme=huge_app,FlashMode=qio,CPUFreq=240`

The CH340 bridge matters when debugging: **toggling RTS resets the board.** Open the port with `DtrEnable`/`RtsEnable` false to attach to a running sketch without restarting it, which is the only way to read state that took a calibration run to build up.

### Pin map

Display (HSPI): DC 2, CS 15, SCK 14, MOSI 13, MISO 12, RST tied high on the PCB (no GPIO), backlight 21. Confirmed by a working `gfx->begin()` and correct colour bars.

Touch (XPT2046, shares VSPI with the card): IRQ 36, MOSI 32, MISO 39, CLK 25, CS 33.

microSD (shares VSPI with touch): MISO 19, MOSI 23, SCK 18, CS 5.

Also present but unexposed to scripts: active-LOW RGB LED (red 4, green 16, blue 17), LDR on 34, speaker on 26. Free GPIOs on the headers: 22, 27, and 35 (input-only). No battery sense circuit, so `sys.battery()` returns null. The SD and audio pins come from community documentation and are **not** independently verified.

## How targets are selected

`.\flash.ps1 -Target waveshare|cyd`. Each target is one entry in the `$Targets` table in `flash.ps1`, holding its FQBN and the `-D` flags that have to reach code which cannot see the sketch's headers.

`board_config.h` is the only file that knows more than one board exists. Everything else reads `BOARD_*` capability flags, so `js-host.ino` contains no board `#ifdef`s. Adding a third board means one `flash.ps1` entry, one pin header, one `board_config.h` block, and a touch driver.

Three values must be `-D` rather than header defines, because their consumers compile outside the sketch:

| Flag | Read by | Why |
|---|---|---|
| `JS_HEAP_CAPS` | `lvgl-js-bindings` | the library cannot include the sketch's `board_config.h` |
| `BOARD_LV_MEM_KB` | LVGL's own sources, via `lv_conf.h` | same |
| `BOARD_WAVESHARE_S3_147` / `BOARD_CYD_2432S028R` | everything | selects the board before anything else is read |

Builds go to `build/<target>/`, and `upload` is pointed at it with `--input-dir`. This is not cosmetic: `arduino-cli upload` rejects `--build-property`, so it cannot re-derive the flag-hashed default build path and would find nothing to upload.

## Memory: the whole problem

Without PSRAM everything competes for one ~320 KB pool. Measured at each boot stage, before any tuning:

```
158776 free after globals
 -51740  WiFi.mode()
 -39276  LVGL (draw buffers, mostly)
 -30204  SD driver
 -80736  QuickJS startup
```

That is ~43 KB short. The interesting part is that **QuickJS consumes essentially the entire remaining pool at startup**, so "it fits" and "a script can run" are different questions.

Rather than drop WiFi — which would cost `wifi.js` and `weather.js` and make the board display-only — the memory comes back from waste:

| Change | Frees | Cost |
|---|---|---|
| `JS_LEAN_CONTEXT`: 12 intrinsic groups → 6 | ~14000 | no Date, Proxy, TypedArrays, WeakRef, atob/btoa, Performance |
| Draw buffers 1/8 → 1/16 screen | 19200 | more flush calls per frame |
| Single-buffered rendering | 19200 | no render/flush overlap |
| `LV_MEM_SIZE` 48 KB → 24 KB | 24576 | smaller LVGL widget pool |

Result: `81076` free at VM start, VM costs `66716`, leaving **22312 bytes for scripts** with WiFi still up.

`JS_NewContext()` loads all twelve intrinsic groups whether a script wants them or not. The six kept are BaseObjects, Eval, RegExp, JSON, MapSet, Promise — chosen by grepping what the shipped scripts actually use. RegExp stays despite no app referencing it directly, because `String.split`/`replace` route through it internally. Adding one back is a line in `new_context()`.

### What that headroom actually buys

| Script | Size | Result |
|---|---|---|
| built-in fallback | ~700 B | runs, including a click handler |
| `app.js` (launcher) | 1518 B | runs, evals in 30 ms |
| `vitals.js` | 4550 B | **fails**: out of memory during eval |

A 4.5 KB script needs well over 14 KB to evaluate, because source text, bytecode and the resulting object graph all exist at once. So small scripts work and the largest app does not.

### Traps worth knowing

**The JS heap needs `MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA`, not bare `MALLOC_CAP_INTERNAL`.** On a classic ESP32 the latter can return IRAM (`0x400xxxxx`), which permits only aligned 32-bit access, while QuickJS writes bytes and shorts across its heap. The first such write panics with `LoadStoreError` / `EXCCAUSE 0x03` inside `JS_NewRuntime2` (observed at `EXCVADDR=0x40091d18`). The DMA cap guarantees byte-addressable DRAM. The S3 never shows this because PSRAM is always DRAM.

**`heap_caps_get_free_size(MALLOC_CAP_INTERNAL)` overstates usable memory by ~58 KB here**, because it counts that unusable IRAM. Measure the DRAM pool when reasoning about JS headroom: 322208 vs 264008 at boot.

**Local statics with non-trivial constructors abort on this target.** `__cxa_guard_acquire` fires the first time the serial REPL is polled, which presents as a boot loop. The REPL's `String` state lives at file scope for this reason.

**Growing the loop-task stack costs JS heap.** Raising `SET_LOOP_TASK_STACK_SIZE` from 32 KB to 48 KB took 16 KB from the same DRAM pool and halved the largest free block (69620 → 31732), breaking context creation. If the JS stack budget needs to change, shrink `JS_MAX_STACK` instead of growing the task.

## The open bug: touch does not read

This is what blocks the target.

**Confirmed working:** the panel and wiring are fine. A standalone probe sketch calibrated to 3.6 px worst error over four points it was not fitted to, and its crosshair test was verified by hand on this board.

**Confirmed under this firmware:** contact reaches the controller. With `-DTOUCH_DEBUG=1` the driver prints Z alongside PENIRQ, and `irq=0` appears for nine consecutive samples while the screen is pressed.

**The failure:** the ADC conversion comes back empty. Z reads 0-1 whether or not anything is touching, so `touch_read()` never reports a press and LVGL never sees a pointer event. Every widget is therefore untappable, which on screen looks like a click that does nothing.

**Measured touch transform**, from the probe's 9-point fit — this is good data, worth keeping when the read path is fixed. A plain transpose, no mirroring, same shape as the Waveshare board:

```
sx = 0.09107 * raw_y - 16.67
sy = 0.06520 * raw_x - 15.79
```

Do not try to re-derive this from the display's rotation bits. Reasoning that way is what produced the original axis bug on the Waveshare board (see [`../lang-c/touch.md`](../lang-c/touch.md)).

`TOUCH_Z_THRESHOLD` must stay at or below 200. A resistive panel presents higher contact resistance at its edges, so corner presses read *weaker* than centre ones; 300 measured fine in the middle while silently rejecting legitimate corner taps.

### What is established about the SPI sharing

The board has three SPI devices and two usable peripherals. The display owns HSPI, so touch and the card share VSPI. FSPI is not a third option: on a classic ESP32 it is bus 1, attached to the flash the firmware executes from.

Two facts about the Arduino SPI layer, both learned the hard way:

- **One `SPIClass` must be shared by both devices.** Each object keeps its own `_spi` handle, so a second object's `begin()` sees `NULL` and calls `spiStartBus()` on an already-running bus.
- **Re-pointing pins must use `spiAttach{SCK,MISO,MOSI}`, not `end()` + `begin()`.** `begin()` alone is a silent no-op on a running bus (`if (_spi) return true;`), but `end()` calls `spiStopBus()`, so the pair tears the peripheral down and rebuilds it around every read.

`shared_spi.h` holds that arbitration. It is arbitration, not locking, and is only sound because of the project's one-task rule: LVGL, touch polling, the JS VM and all filesystem access run on the loop task, so a claim cannot be preempted mid-transfer. The `jsfetch` worker is the one other task and never touches SPI.

### Dead ends, so they are not retried

| Theory | Why it was wrong |
|---|---|
| JS stack limit too large for the task stack | disproved by `30680 bytes unused` against a 12288 limit |
| `qjs_usable_size` returning 0 breaks malloc accounting | `malloc_limit` is 0 = unlimited, so the check cannot trip |
| Passing CS as `begin()`'s `ss` makes it a hardware chip-select | `begin()` only records `_ss`; `setHwCs()` is opt-in and never called |
| `end()` + `begin()` is the correct way to re-assert pins | it works, but `spiStopBus()` restarts the bus around every ADC read |
| LVGL fonts waste RAM | fonts are `const` glyph data in **flash**; trimming saves flash, not DRAM |

### The next step

Build with `-DBOARD_SKIP_SD_TEST=1`, which leaves the card unmounted so nothing shares the bus, and check whether touch reads. If it works, the fault is in the sharing. If it does not, the fault is in `touch_xpt2046.cpp` independently of the card, which is a much smaller search space. `TOUCH_DEBUG` prints Z with PENIRQ for exactly that comparison.

Worth noting when interpreting results: a run showing `irq=1` throughout means nothing was touching the screen, so it says nothing about whether the read path works. Confirm contact was registered before drawing conclusions from a Z value.

## What an ESP32 needs to run this launcher

Generalising from the two boards measured. The short version: **PSRAM is the only spec that really decides this**, because internal SRAM is uniformly small across the entire ESP32 family and no variant has enough of it to be comfortable.

Internal SRAM by SoC, for scale ([Espressif SoC comparison](https://www.espboards.dev/blog/esp32-soc-options/)):

| SoC | Arch | Internal SRAM | PSRAM | Verdict for this runtime |
|---|---|---|---|---|
| ESP32 (classic) | dual Xtensa LX6 | **520 KB** | external, WROVER variants | best case *without* PSRAM — and still tight (measured) |
| ESP32-S3 | dual Xtensa LX7 | 512 KB | external, common (R2/R8) | **the comfortable target** with PSRAM (measured) |
| ESP32-S2 | single LX7 | 320 KB | external, some modules | workable with PSRAM; single-core |
| ESP32-C6 | single RISC-V | 512 KB | no | small scripts only, if at all |
| ESP32-C3 | single RISC-V | 400 KB | no | marginal |
| ESP32-C5 | single RISC-V | 384 KB | external supported | fine with PSRAM |
| ESP32-C2 | single RISC-V | 272 KB | no | no |
| ESP32-H2 | single RISC-V | 256 KB | no | no, and no WiFi |
| ESP32-P4 | dual RISC-V 400 MHz | 768 KB | external supported | plenty, but no built-in WiFi |

The classic ESP32 has the **most** internal SRAM of any WiFi-capable variant here, and on it the JS heap still ended up with 22 KB for scripts after everything else took its share. That is the useful conclusion: if the best PSRAM-less case is this tight, no PSRAM-less variant does meaningfully better. The spread from 256 KB to 520 KB does not change the answer, because the fixed costs (WiFi ~52 KB, LVGL ~39 KB, SD ~30 KB, QuickJS ~67-81 KB) dominate.

### Concrete requirements

**PSRAM: any amount, and the single thing worth checking first.** With it (8 MB on the S3 board) the JS heap is effectively unlimited for UI work and every app runs. Without it you are budgeting in single-digit KB and large scripts fail. Note the module suffix rather than the SoC name: `ESP32-S3` alone says nothing, `ESP32-S3R8` means 8 MB octal PSRAM.

**Usable DRAM matters more than the SRAM figure on the datasheet.** Of the classic ESP32's 520 KB, only 264008 bytes were byte-addressable DRAM free at boot — the rest is IRAM (which QuickJS cannot use, see the trap above) and statically allocated. Budget from the DRAM figure.

**Flash: 4 MB minimum, 8-16 MB to be comfortable.** This firmware is ~2 MB, so 4 MB works only with a `huge_app`-style 3 MB app partition and leaves no room for a script partition — scripts then have to come from an SD card. 16 MB allows the 9.9 MB FATFS the Waveshare target uses, which is what makes `flash:` paths and card-free operation possible.

**WiFi, if scripts need `fetch`.** That rules out the H2 and P4 as single-chip solutions. It also costs ~52 KB of DRAM, which is the largest single line item on a PSRAM-less board.

**Two spare SPI peripherals, or a non-SPI touch controller.** Classic ESP32 and S3 expose two usable SPI buses (the third is wired to flash). A board with an SPI display *and* SPI touch *and* an SPI card has three devices for two buses, which is what created the bus-sharing problem documented above. I2C touch (like the Waveshare's AXS5106L) avoids this entirely.

**LVGL 9.x and the Arduino ESP32 core**, unchanged from [portability.md](portability.md).

### Rules of thumb

- **PSRAM present** → everything works; treat internal RAM as irrelevant.
- **No PSRAM, ≥400 KB SRAM** → the runtime starts and small scripts run, with per-target tuning (lean context, single draw buffer, reduced `LV_MEM_SIZE`). Expect to hand-tune and to keep scripts under ~2 KB.
- **No PSRAM, <400 KB SRAM** → not worth attempting.
- **RISC-V variants** are untested here. QuickJS-ng compiled for Xtensa LX6 with zero source changes, so the engine is unlikely to be the obstacle, but the vendored Xtensa patches ([engine-notes.md](engine-notes.md)) mean the build has only been exercised on Xtensa.

## Also fixed along the way

`writeScript()` did not create parent directories, so `app-begin /apps/x.js` failed on any card without an existing `/apps` folder, reporting only "write FAILED". This affected both boards, not just the CYD.

## What remains

- **Touch read path** — blocks everything; nothing can be tapped.
- **`vitals.js` does not fit** — needs either more DRAM (trimming unused widgets from `lv_conf.h` *is* static RAM, unlike fonts) or a smaller script.
- **Responsive layout** — the apps use fixed pixel sizes tuned for 320×172, so they fit 320×240 without clipping but leave dead space. The binding layer already supports `"50%"` sizes and flex, so this is a script-only change. `selftest.js` should keep its fixed sizes, which are test fixtures rather than layout.
- **The Waveshare target has not been re-run on hardware** since the restructuring. It compiles, and its flags are unchanged, but that is not the same as verified.
- **`lang-c` is deliberately not ported.** It stays an S3-only reference, so CLAUDE.md's "fix hardware bugs in `lang-c` first, then re-copy" rule cannot apply to CYD glue, which exists only under `lang-js`.
