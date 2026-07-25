# Build & deploy — the JavaScript way

The JS-specific half of [`BUILDING.md`](../../BUILDING.md): building the JsHost firmware and getting `app.js` onto the board. Toolchain setup, the FQBN, and general USB/serial troubleshooting are shared with the C way — see [`BUILDING.md`](../../BUILDING.md) and [`lang-c/build-and-flash.md`](../lang-c/build-and-flash.md).

## Building JsHost

`lang-js\flash.ps1` wraps everything (`-BuildOnly`, `-Port COMx`, `-Monitor` as usual). The one difference from the C build: the vendored QuickJS-ng engine is an Arduino library at `lang-js/quickjs-ng/`, linked explicitly:

```powershell
$fqbn = 'esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=cdc,FlashMode=qio,USBMode=hwcdc'
arduino-cli compile --library .\lang-js\quickjs-ng -b $fqbn .\lang-js\JsHost
arduino-cli upload  -b $fqbn -p COM4 .\lang-js\JsHost
```

Forgetting `--library` fails fast with `quickjs.h: No such file or directory`. The `app3M_fat9M_16MB` partition scheme matters doubly here: it also provides the 9.9 MB FATFS partition the runtime can load scripts from.

## Deploying app.js

Flashing JsHost is a one-time step; after that the UI is data, not firmware. The boot search order is `sd:/app.js` → `ffat:/app.js` → a built-in "no app.js found" fallback screen. Two ways to ship a script:

- **SD card:** copy [`lang-js/app/app.js`](../../lang-js/app/app.js) to the root of a FAT-formatted microSD, insert it, long-press **BOOT** (≥ 700 ms). The card is re-mounted on every reload, so it can be swapped while the board is powered, and it always wins over the flash partition.
- **Over serial:** send the line `app-begin`, then the script's lines, then `app-end`. JsHost writes the script to the internal FATFS partition and reloads immediately — no card handling, works from any terminal or script talking to the COM port at 115200.

## Serial commands & the REPL

While JsHost runs, the serial port accepts one line at a time:

| Input | Effect |
|---|---|
| `reload` | tear down the JS world and re-read app.js from storage (same as BOOT long-press) |
| `app-begin` … `app-end` | receive a script into `ffat:/app.js`, then reload (256 KB cap) |
| `app-clear` | delete `ffat:/app.js` and reload (back to SD or the fallback) |
| anything else | evaluated as JavaScript in the running app; result or exception prints back |

The REPL shares the app's global scope, so `sys.heap()`, poking widgets held in top-level `const`s, or arming a quick `lv.timer` all work live.

## Expected boot log

```
[boot] JsHost starting
[boot] display 320x172
[touch] AXS5106L ok, id = 51 06 01
[app] running ffat:/app.js
app.js: vitals dashboard up (4 tabs)
[js] ffat:/app.js: eval ok in 74 ms
[boot] ready — serial is a JS REPL; 'reload' = reload app.js
```

A script that throws at boot is reported over serial (message + stack) and replaced by the fallback screen; the firmware never goes down with the script. Engine-level traps (heap poisoning vs `usable_size`, the promise job pump, the DTR/RTS bootloader trap) live in [`engine-notes.md`](engine-notes.md).
