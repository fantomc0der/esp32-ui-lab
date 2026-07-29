# Build & deploy

Building the firmware and getting `app.js` onto the board. Toolchain setup, the FQBN, and general USB/serial troubleshooting are in [`BUILDING.md`](../BUILDING.md); the frozen C dashboard has its own notes in [`experiments/c-dashboard/build-and-flash.md`](experiments/c-dashboard/build-and-flash.md).

## Building the firmware

`flash.ps1` at the repo root wraps everything (`-BuildOnly`, `-Port COMx`, `-Monitor` as usual, plus `-Board` to pick one of `firmware/boards/*`). What it does that a bare compile does not: the two Arduino libraries live in `firmware/` rather than in the Arduino libraries folder, so both are linked explicitly:

```powershell
$fqbn = 'esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=cdc,FlashMode=qio,USBMode=hwcdc'
arduino-cli compile --library .\firmware\quickjs-ng --library .\firmware\lvgl-js-bindings -b $fqbn .\firmware\boards\waveshare-s3-touch-147
arduino-cli upload  -b $fqbn -p COM4 .\firmware\boards\waveshare-s3-touch-147
```

Omitting either `--library` fails fast, with `js_bindings.h: No such file or directory` for the bindings or `quickjs.h` for the engine. The `app3M_fat9M_16MB` partition scheme matters doubly here: it also provides the 9.9 MB FATFS partition the runtime can load scripts from.

Both libraries compile against the **sketch-local** `lv_conf.h`, because the sketch folder is on the include path for library units. A new board sketch therefore needs its own `lv_conf.h`, or LVGL and the bindings will disagree about its configuration.

## Deploying app.js

Flashing is a one-time step; after that the UI is data, not firmware. The board boots `/app.js` — the launcher — which lists `/apps/*.js` and runs whichever you tap. An app that is missing or throws falls back to the launcher, and a missing launcher falls back to a screen built into the firmware, so the panel is never dead. Paths prefer the SD card and fall back to the flash partition, so the same layout works with or without a card fitted.

To ship one app rather than a menu, pin it: long-press its row in the launcher (or send `pin /apps/clock.js`). The board then boots straight into that script with nothing drawn over it. A long-press of **BOOT** still opens the launcher, which is where you release the pin.

Pinning changes what boots and what the firmware draws over it; it does not stop other apps running. The launcher still lists everything in `/apps` and still launches it, so a pinned board is an appliance by default rather than by restriction. In the launcher, a tap runs an app without touching the pin, and a long-press moves the pin to that app, replacing whatever was pinned before with no confirmation. Full rules, including the back arrow you get after tapping an app on a pinned board: [Pinning one app](binding-api.md#pinning-one-app).

One thing a pinned board still draws in that corner: if the pinned app asks about the network and none is set up, a Wi-Fi button appears and opens `/apps/wifi.js`, with a back arrow to return. Pin an app that needs the network on a board you have not joined to one, and you can still fix that from the touchscreen.

Two ways to ship a script:

- **SD card:** copy [`app/app.js`](../app/app.js) to the root of a FAT-formatted microSD, insert it, long-press **BOOT** (≥ 700 ms). The card always wins over the flash partition. It is mounted once in `setup()` and handed to the supervisor, so swapping cards needs a reset rather than just a reload: a reload re-reads the file, not the filesystem.
- **Over serial:** send the line `app-begin`, then the script's lines, then `app-end`. The board writes the script and reloads immediately — no card handling, works from any terminal or script talking to the COM port at 115200.

[`push.ps1`](../push.ps1) drives the serial route, which is worth using rather than pasting by hand:

```powershell
# from the repo root
.\push.ps1 app\apps\weather.js         # -> /apps/weather.js, then reloads it
.\push.ps1 app\app.js                  # -> /app.js
.\push.ps1 app\selftest.js -Dest /app.js
```

An app written in JSX has to be built first, since the board parses plain JavaScript. The output is committed, so this only matters when you have edited the source:

```powershell
node tools/build-app.mjs tasks         # app\src\tasks.jsx -> app\apps\tasks.js
.\push.ps1 app\apps\tasks.js
```

The destination mirrors the repo layout under `app\`, because that layout *is* the card layout.

It exists because both ways this route goes wrong leave a file that still parses, so the board reloads without complaint and the damage only shows on the panel: the encoding trap under [Serial commands & the REPL](#serial-commands--the-repl) below, and pacing — the host reads serial one character at a time from `loop()`, so a file pushed as fast as the port accepts it loses bursts out of the middle. `push.ps1` forces UTF-8, paces the lines, and then reads the file back off the board and compares a position-sensitive checksum against the local copy, so a mangled push fails loudly. Raise `-LineDelayMs` if a mismatch ever repeats.

Note that the stored copy always ends with exactly one more newline than the file on disk: the protocol writes `line + "\n"` for every line, including the last. That is expected and is what the checksum accounts for.

## Checking the binding layer

[`app/selftest.js`](../app/selftest.js) is a script you deploy exactly like `app.js`. It exercises the binding surface and prints a `PASS`/`FAIL` line per check, ending with a count:

```
SELFTEST 66 passed, 0 failed
```

Anything other than `0 failed` means the bindings regressed. It covers widget construction, props, values, chaining, misuse rejection, `sys.*`, `fs.*`, timers, promise resolution, and the `.delete()`/`.index()` pair the component runtime is built on, plus regression checks for two use-after-free bugs that were found on hardware (a timer callback stopping its own timer, and a widget handle used after its container was cleaned). Touch-driven behaviour is not covered, since a tap cannot be synthesized, and neither is anything needing a network. Run it after any change to the binding library, then re-deploy `app.js`.

It is also the only functional test of the binding layer, and it cannot run in CI: it executes on the board and reports over serial. CI syntax-checks every script and verifies they only call bindings the C layer registers ([`tools/check-js-api.mjs`](../tools/check-js-api.mjs)), which catches a typo or a removed binding but not a behavioural regression. Now that the supervisor lives in the library rather than in each sketch, a bug there reaches every board, which makes running this by hand after binding-layer changes the actual gate.

## Checking the component runtime

[`app/ui-selftest.js`](../app/ui-selftest.js) does the same job for the JSX layer, and is deployed the same way:

```powershell
node tools/build-app.mjs ui-selftest
.\push.ps1 app\ui-selftest.js -Dest /app.js
```

```
UISELFTEST 17 passed, 0 failed
```

Unlike the binding selftest, most of this ground *is* covered without hardware — [`tools/test-ui.mjs`](../tools/test-ui.mjs) runs the reconciler against a fake `lv` in CI, and covers more cases than this does, since a mock can count widget creations and inspect the props of each `.set()`. What only the board can show is that the bookkeeping matches LVGL: that `lv_obj_move_to_index` really reorders, that a deleted widget really invalidates its handle, and that a render deferred into a promise microtask really lands within a frame. Run it after changing [`app/lib/ui.js`](../app/lib/ui.js), then put the launcher back with `.\push.ps1 app\app.js`.

## Serial commands & the REPL

While the firmware runs, the serial port accepts one line at a time:

| Input | Effect |
|---|---|
| `home` | open the launcher (same as the on-screen home button, or a BOOT long-press) |
| `reload` | restart the current app from storage |
| `pin [path]` | boot straight into this app from now on and stop drawing over it. With no path it pins whatever is running. Replaces any existing pin, and refuses to pin the launcher |
| `unpin` | back to booting the launcher |
| `ls [dir]` | list a directory, default `/` |
| `rm <path>` | delete a file |
| `app-begin [path]` … `app-end` | receive a script and write it, then run it. With no path it replaces whatever is running, which is the usual edit loop; give a path to add a new app, e.g. `app-begin /apps/clock.js`. Lines are capped at 4 KB and the whole transfer at 256 KB, but the transfer buffer is internal RAM, so a large upload fails on allocation long before that. Any of the three aborts the upload rather than writing a truncated script |
| anything else | evaluated as JavaScript in the running app; result or exception prints back |

Paths are on the SD card, or on the flash partition with a `flash:` prefix.

**Send UTF-8.** The upload is a plain byte stream, so a terminal that transmits ASCII silently replaces every non-ASCII character with `?` — a `°` in a script becomes `?` on screen, and nothing reports an error. In PowerShell that means setting the encoding explicitly, since `SerialPort` defaults to ASCII:

```powershell
$port.Encoding = [System.Text.Encoding]::UTF8
Get-Content script.js -Encoding UTF8 | ForEach-Object { $port.WriteLine($_) }
```

Copying the file onto the SD card sidesteps this entirely, and so does [`push.ps1`](../push.ps1), which sets the encoding and then verifies the transfer. The compiled fonts do cover Latin-1, so `°` renders fine once it actually arrives intact.

Worth knowing that this section existing was not enough: the trap was documented here and still cost an afternoon, because the natural way to script the upload (.NET `SerialPort`) defaults to ASCII and reports nothing. That is the argument for using the script rather than rolling the loop again.

The REPL shares the app's global scope, so `sys.heap()`, poking widgets held in top-level `const`s, or arming a quick `lv.timer` all work live.

## Expected boot log

```
[boot] waveshare-s3-touch-147 starting
[boot] display 320x172
[touch] AXS5106L ok, id = 51 06 01
[app] storage: sd ok, flash ok
[app] running /app.js
[js] vm ready: 90048 bytes to start, 8203788 free for scripts
launcher: 3 app(s) found
[js] /app.js: eval ok in 74 ms
[app] ready — serial is a JS REPL; 'reload' restarts the app, 'home' opens the launcher
```

The `[boot]` lines come from the sketch and the `[app]`/`[js]` lines from the library, which is the seam visible in the log: everything after storage is mounted is board-independent. On a pinned board the launcher line is replaced by `[app] <path> is pinned — skipping the launcher`.

A script that throws at boot is reported over serial (message + stack) and replaced by the fallback screen; the firmware never goes down with the script. Engine-level traps (heap poisoning vs `usable_size`, the promise job pump, the DTR/RTS bootloader trap) live in [`engine-notes.md`](engine-notes.md).
