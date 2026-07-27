# waveshare-s3-touch-147

The board sketch for the Waveshare ESP32-S3-Touch-LCD-1.47 (ESP32-S3R8, 172×320 IPS, JD9853 controller, AXS5106L touch). It brings up the display, touch, and LVGL, then hands the screen to a JavaScript file run by the [QuickJS-ng engine](../../quickjs-ng/README.md) through the [LVGL binding library](../../lvgl-js-bindings/README.md).

This is the board-specific half of the firmware, and only that: pinout, hardware bring-up, the three host hooks the bindings call, and a `JsvmAppConfig` naming the launcher, the Wi-Fi app, and the button. Everything about *running* scripts is board-independent policy owned by the binding library's supervisor, so it is not here to be copied and drifted. That is why this sketch is 189 lines. The reusable half lives in the two libraries in [`firmware/`](../../README.md); what a sketch for a different board has to supply is in [`docs/portability.md`](../../../docs/portability.md).

Two docs cover the layers above: [`docs/binding-api.md`](../../../docs/binding-api.md) is the script author's reference (what `app.js` can call), and [`docs/runtime-architecture.md`](../../../docs/runtime-architecture.md) is the maintainer's (how the bindings, ownership rules, and reload path actually work). Read the second one before changing the binding library.

## Where the script comes from (boot order)

1. `/app.js` on the microSD card (SD_MMC 4-bit)
2. `/app.js` on the 9.9 MB FATFS flash partition (formatted automatically on first use)
3. a built-in fallback screen that says "no app.js found" — also shown when the loaded script throws at boot

Both filesystems are mounted once in `setup()` and handed to the supervisor, which is what gives scripts a real `fs` API. The tradeoff: swapping cards needs a reset.

The shipped app lives at [`app/app.js`](../../../app/app.js); copy it to a FAT-formatted card's root.

Pinning replaces step 1: with a pin set (long-press an app in the launcher, or `pin <path>` over serial), the boot goes straight to that script and the firmware draws nothing over it, so the board reads as a single-app appliance rather than a menu. A long-press of **BOOT** still opens the launcher, which is the only way back once the corner button is gone, and where you unpin.

It reads as an appliance rather than being locked into one. The launcher still runs any app you tap there, and a long-press retargets the pin to that app, replacing the previous one without asking. See [Pinning one app](../../../docs/binding-api.md#pinning-one-app) for the full rules.

## The corner button

The supervisor owns the bottom-right corner and puts at most one control there, chosen from what is running, whether a pin is set, and whether the running app wants a network the board has not got: **home** to the launcher, **back** to the pinned app, **Wi-Fi** to `/apps/wifi.js`, or nothing when you are already where the button would take you. This sketch's only say in it is naming the two destinations in its `JsvmAppConfig`. The Wi-Fi state is the reason a board pinned to a network app is not a dead end when no network was ever set up; [`docs/runtime-architecture.md`](../../../docs/runtime-architecture.md) covers how the firmware infers that an app wants one.

## The edit loop

Edit `app.js` on the PC → put the card back → long-press **BOOT** (≥ 700 ms). The JS world is torn down, the file is re-read, the new UI builds. No toolchain, no reflash.

Two extras over serial (115200): typing `reload` triggers the same reload as the button, and any other line is evaluated as JavaScript inside the running app (a live REPL — `sys.heap()`, poking widgets, etc.).

## Build & flash

```powershell
# from the repo root
.\flash.ps1              # compile this board (links both libraries), upload, monitor
```

The hardware files (`board_pins.h`, `jd9853_panel.h`, `axs5106l_touch.*`, `lv_conf.h`) started as copies of the frozen [`c-dashboard`](../../../experiments/c-dashboard/README.md), where they were first proven against hardware, and are now maintained here. `lv_conf.h` diverges from that copy on purpose in two lines: Montserrat 28 and 40 are enabled, because scripts can select those sizes.
