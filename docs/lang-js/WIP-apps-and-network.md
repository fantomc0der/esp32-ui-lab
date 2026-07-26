# WIP: apps and network (branch `apps-and-network`)

Working notes for the in-progress branch, so a cold session can pick this up. Delete this file when the PR merges.

## Goal

Turn the board from one-script-one-purpose into a launcher plus several apps, and give scripts filesystem and network access. New app that looks nothing like the tabbed dashboard.

## Done

- [x] **`fs` bindings** — `bindings_fs.cpp`. read/write/append/list/exists/remove/mkdir/isDir/available over an `fs::FS` the host registers via `jsvm_set_filesystem()`. Unprefixed paths prefer SD and fall back to flash; `flash:` prefix forces flash. 256 KB read cap. *Verified: mkdir/write/read/remove round-trip on hardware.*
- [x] **App switching** — `sys.launch(name)` records a request; host collects it with `jsvm_take_pending_launch()` and switches from `loop()`. Cannot be synchronous: tearing down the context mid-call frees the running closure. *Verified: 3 launcher↔app round trips, heap byte-identical.*
- [x] **Home button** — drawn by the firmware on `lv_layer_top()`, which is not a child of the active screen, so `lv_obj_clean()` can't delete it and no script can reach it. Hidden in the launcher. BOOT long-press is the hardware fallback. *Not yet verified with a finger.*
- [x] **Persistent storage mount** — SD and FFat mount once at boot and stay mounted. Cost: card swap now needs a reset. This revealed the SD card had never actually been read before; the old per-load mount always failed and silently used flash.
- [x] **WiFi credentials** — NVS via `Preferences`, namespace `jsvm-wifi`. `wifi.save/status/connect/forget`. Write-only from JS: no API returns the password. Host calls `jsvm_wifi_autoconnect()` at boot.
- [x] **Reconnect** — `WiFi.onEvent` + `WiFi.reconnect()` on disconnect. The ESP32 does not retry on its own.
- [x] **`fetch(url)`** — real Promise. Worker FreeRTOS task (16 KB stack, TLS via `setInsecure`), result returned through a FreeRTOS queue drained by an `lv_timer`, so the promise settles on `loopTask`. Abandoned requests are handled with a generation counter rather than killing the worker. 128 KB body cap. *Verified: guard when disconnected. Live request needs credentials.*
- [x] **Keyboard/textarea bindings** — `lv.textarea` (placeholder/password/oneLine/maxLength), `lv.keyboard` + `.target(ta)`, `ready`/`cancel` events, `.value()` reads textarea text.
- [x] **Bigger fonts** — montserrat 28 and 40 enabled; `font_by_size()` accepts them.
- [x] **Launcher** — `app/app.js` lists `/apps/*.js`, tap to launch. Empty state tells you how to add one.
- [x] **Apps written** — `apps/vitals.js` (renamed from the old app.js), `apps/wifi.js` (scan → keyboard → save), `apps/weather.js` (fullscreen big type, fetch + fs cache + config).

Commits so far: `cd44c29` (fs + switching + home), `649bf5c` (wifi + fetch).

## Remaining

- [x] **Deployed and verified** — all three apps upload, run, and switch cleanly; heap drift across 6 switches was 288 B internal / 140 B PSRAM.
- [ ] **Extend `selftest.js`** for `fs.*`, `sys.launch`, `wifi.status`, textarea/keyboard.
- [ ] **Docs** — `binding-api.md` (fs, fetch, wifi, textarea/keyboard, new fonts), `architecture.md` (the deferred-switch rule, worker-task pattern, `lv_layer_top` ownership), `build-and-deploy.md` (new serial commands: `home`, `ls`, `rm`, `app-begin <path>`).
- [ ] **Open the PR** against `main`.

## Needs the user (cannot be done from here)

- Tapping anything: the home button, tab swiping, the wifi app's keyboard, weather's tap-to-refresh.
- Real WiFi credentials, which gates any live `fetch()` test. Enter them via the Wifi app on the device.

## Gotchas discovered

- Rebuilding a screen from inside a click handler deletes the widget LVGL is dispatching to. Apps defer with a 20 ms `lv.timer` (see the `next()` helper in `wifi.js`). Worth documenting as the standard pattern.
- The home button sits bottom-right, so full-width bottom widgets collide with it. `wifi.js` sizes its keyboard to 280 px to leave the corner clear.
