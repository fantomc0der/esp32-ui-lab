# JsHost — the JavaScript runtime firmware

The firmware is C; the UI is not. JsHost brings up the display/touch/LVGL stack exactly like the hardware-proven C demo ([`lang-c/WaveshareVitals`](../../lang-c/WaveshareVitals/)), then hands the screen to a JavaScript file run by the [QuickJS-ng engine](../quickjs-ng/README.md) through the [LVGL binding library](../lv-binding-js-esp32/README.md).

This sketch is the board-specific half: hardware bring-up, the three host hooks the bindings call, and the policy choices (where scripts come from, what triggers a reload, what the serial port does). The reusable half lives in the two libraries beside it.

Two docs cover this sketch: [`docs/lang-js/binding-api.md`](../../docs/lang-js/binding-api.md) is the script author's reference (what `app.js` can call), and [`docs/lang-js/architecture.md`](../../docs/lang-js/architecture.md) is the maintainer's (how the bindings, ownership rules, and reload path actually work). Read the second one before changing the binding library.

## Where the script comes from (boot order)

1. `/app.js` on the microSD card (SD_MMC 4-bit; the card is mounted fresh on every load so it can be swapped while powered)
2. `/app.js` on the 9.9 MB FATFS flash partition (formatted automatically on first use)
3. a built-in fallback screen (`js_fallback.h`) that says "no app.js found" — also shown when the loaded script throws at boot

The shipped app lives at [`../app/app.js`](../app/app.js); copy it to a FAT-formatted card's root.

## The edit loop

Edit `app.js` on the PC → put the card back → long-press **BOOT** (≥ 700 ms). The JS world is torn down, the file is re-read, the new UI builds. No toolchain, no reflash.

Two extras over serial (115200): typing `reload` triggers the same reload as the button, and any other line is evaluated as JavaScript inside the running app (a live REPL — `sys.heap()`, poking widgets, etc.).

## Build & flash

```powershell
cd lang-js
.\flash.ps1              # compile JsHost (links both libraries), upload, monitor
```

Hardware files (`board_pins.h`, `jd9853_panel.h`, `axs5106l_touch.*`, `lv_conf.h`) are verbatim copies from `lang-c/WaveshareVitals` — fix bugs there first, then re-copy.
