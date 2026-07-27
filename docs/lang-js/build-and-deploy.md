# Build & deploy — JavaScript

The JS-specific half of [`BUILDING.md`](../../BUILDING.md): building the js-host firmware and getting `app.js` onto the board. Toolchain setup, the FQBN, and general USB/serial troubleshooting are shared with the C sketch — see [`BUILDING.md`](../../BUILDING.md) and [`lang-c/build-and-flash.md`](../lang-c/build-and-flash.md).

## Building js-host

`lang-js\flash.ps1` wraps everything (`-BuildOnly`, `-Port COMx`, `-Monitor` as usual). The one difference from the C build: two Arduino libraries sit beside the sketch rather than inside it, the vendored engine at `lang-js/quickjs-ng/` and the bindings at `lang-js/lvgl-js-bindings/`, and both are linked explicitly:

```powershell
$fqbn = 'esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=cdc,FlashMode=qio,USBMode=hwcdc'
arduino-cli compile --library .\lang-js\quickjs-ng --library .\lang-js\lvgl-js-bindings -b $fqbn .\lang-js\js-host
arduino-cli upload  -b $fqbn -p COM4 .\lang-js\js-host
```

Omitting either `--library` fails fast, with `js_bindings.h: No such file or directory` for the bindings or `quickjs.h` for the engine. The `app3M_fat9M_16MB` partition scheme matters doubly here: it also provides the 9.9 MB FATFS partition the runtime can load scripts from.

Both libraries compile against the **sketch-local** `lv_conf.h`, because the sketch folder is on the include path for library units. If you copy js-host as a starting point for another board, keep `lv_conf.h` in the sketch folder or LVGL and the bindings will disagree about its configuration.

## Deploying app.js

Flashing js-host is a one-time step; after that the UI is data, not firmware. The board boots `/app.js` — the launcher — which lists `/apps/*.js` and runs whichever you tap. An app that is missing or throws falls back to the launcher, and a missing launcher falls back to a screen built into the firmware, so the panel is never dead. Paths prefer the SD card and fall back to the flash partition, so the same layout works with or without a card fitted.

To ship one app rather than a menu, pin it: long-press its row in the launcher (or send `pin /apps/clock.js`). The board then boots straight into that script with no home button drawn over it. A long-press of **BOOT** still opens the launcher, which is where you release the pin.

Two ways to ship a script:

- **SD card:** copy [`lang-js/app/app.js`](../../lang-js/app/app.js) to the root of a FAT-formatted microSD, insert it, long-press **BOOT** (≥ 700 ms). The card is re-mounted on every reload, so it can be swapped while the board is powered, and it always wins over the flash partition.
- **Over serial:** send the line `app-begin`, then the script's lines, then `app-end`. js-host writes the script and reloads immediately — no card handling, works from any terminal or script talking to the COM port at 115200.

[`lang-js/push.ps1`](../../lang-js/push.ps1) drives the serial route, which is worth using rather than pasting by hand:

```powershell
cd lang-js
.\push.ps1 app\apps\weather.js         # -> /apps/weather.js, then reloads it
.\push.ps1 app\app.js                  # -> /app.js
.\push.ps1 app\selftest.js -Dest /app.js
```

The destination mirrors the repo layout under `app\`, because that layout *is* the card layout.

It exists because both ways this route goes wrong leave a file that still parses, so the board reloads without complaint and the damage only shows on the panel: the encoding trap under [Serial commands & the REPL](#serial-commands--the-repl) below, and pacing — the host reads serial one character at a time from `loop()`, so a file pushed as fast as the port accepts it loses bursts out of the middle. `push.ps1` forces UTF-8, paces the lines, and then reads the file back off the board and compares a position-sensitive checksum against the local copy, so a mangled push fails loudly. Raise `-LineDelayMs` if a mismatch ever repeats.

Note that the stored copy always ends with exactly one more newline than the file on disk: the protocol writes `line + "\n"` for every line, including the last. That is expected and is what the checksum accounts for.

## Checking the binding layer

[`lang-js/app/selftest.js`](../../lang-js/app/selftest.js) is a script you deploy exactly like `app.js`. It exercises the binding surface and prints a `PASS`/`FAIL` line per check, ending with a count:

```
SELFTEST 35 passed, 0 failed
```

Anything other than `0 failed` means the bindings regressed. It covers widget construction, props, values, chaining, misuse rejection, `sys.*`, timers, and promise resolution, plus regression checks for two use-after-free bugs that were found on hardware (a timer callback stopping its own timer, and a widget handle used after its container was cleaned). Touch-driven behaviour is not covered, since a tap cannot be synthesized. Run it after any change to the binding library, then re-deploy `app.js`.

## Serial commands & the REPL

While js-host runs, the serial port accepts one line at a time:

| Input | Effect |
|---|---|
| `home` | open the launcher (same as the on-screen home button, or a BOOT long-press) |
| `reload` | restart the current app from storage |
| `pin [path]` | boot straight into this app from now on and drop the home button. With no path it pins whatever is running |
| `unpin` | back to booting the launcher |
| `ls [dir]` | list a directory, default `/` |
| `rm <path>` | delete a file |
| `app-begin [path]` … `app-end` | receive a script and write it, then run it. With no path it replaces whatever is running, which is the usual edit loop; give a path to add a new app, e.g. `app-begin /apps/clock.js`. 256 KB cap |
| anything else | evaluated as JavaScript in the running app; result or exception prints back |

Paths are on the SD card, or on the flash partition with a `flash:` prefix.

**Send UTF-8.** The upload is a plain byte stream, so a terminal that transmits ASCII silently replaces every non-ASCII character with `?` — a `°` in a script becomes `?` on screen, and nothing reports an error. In PowerShell that means setting the encoding explicitly, since `SerialPort` defaults to ASCII:

```powershell
$port.Encoding = [System.Text.Encoding]::UTF8
Get-Content script.js -Encoding UTF8 | ForEach-Object { $port.WriteLine($_) }
```

Copying the file onto the SD card sidesteps this entirely, and so does [`push.ps1`](../../lang-js/push.ps1), which sets the encoding and then verifies the transfer. The compiled fonts do cover Latin-1, so `°` renders fine once it actually arrives intact.

Worth knowing that this section existing was not enough: the trap was documented here and still cost an afternoon, because the natural way to script the upload (.NET `SerialPort`) defaults to ASCII and reports nothing. That is the argument for using the script rather than rolling the loop again.

The REPL shares the app's global scope, so `sys.heap()`, poking widgets held in top-level `const`s, or arming a quick `lv.timer` all work live.

## Expected boot log

```
[boot] js-host starting
[boot] display 320x172
[touch] AXS5106L ok, id = 51 06 01
[app] running ffat:/app.js
app.js: vitals dashboard up (4 tabs)
[js] ffat:/app.js: eval ok in 74 ms
[boot] ready — serial is a JS REPL; 'reload' = reload app.js
```

A script that throws at boot is reported over serial (message + stack) and replaced by the fallback screen; the firmware never goes down with the script. Engine-level traps (heap poisoning vs `usable_size`, the promise job pump, the DTR/RTS bootloader trap) live in [`engine-notes.md`](engine-notes.md).
