# The host build: testing the C layer without a board

`firmware/host-test/` compiles the binding layer for the machine you are sitting at, links it against stubs for the Arduino surface, and runs it under AddressSanitizer and UndefinedBehaviorSanitizer. It exists because everything correctness-critical in `firmware/lvgl-js-bindings/` was previously verified only by `app/selftest.js`, which runs on the panel and reports over serial — so CI never compiled a line of the C layer, and a memory bug waited until somebody flashed a board.

The two use-after-frees this repo has actually hit are the argument for it. A stale widget handle written through, and a trampoline dropping the last reference to a closure it was mid-call on (both in [`runtime-architecture.md`](runtime-architecture.md)) are precisely the class of bug a sanitizer catches on every commit and a human reading a serial log does not. Both are now regression cases in `tests/test_ownership.cpp`.

## Running it

Needs a C++17 compiler, CMake ≥ 3.22 and Ninja. Nothing else: LVGL is fetched by the build, QuickJS is already vendored, and no display libraries are involved.

```bash
cd firmware/host-test
cmake -S . -B .build -G Ninja
cmake --build .build -j
cd .build && ctest --output-on-failure
```

The first configure clones LVGL 9.5.0, so it needs network and takes a minute; after that a full rebuild is seconds. `.build/` is gitignored.

On Windows, where the repo is normally developed, there is no system compiler. A container is the path of least resistance and matches what CI runs:

```powershell
podman run --rm -v "${PWD}:/w" -w /w ubuntu:24.04 bash -lc `
  "apt-get update -qq && apt-get install -y -qq build-essential cmake ninja-build git && cmake -S firmware/host-test -B /tmp/b -G Ninja && cmake --build /tmp/b -j && ctest --test-dir /tmp/b --output-on-failure"
```

Building into `/tmp/b` rather than into the mount keeps the object tree inside the container, so a Linux build tree is not left behind in a Windows working copy. Drop the `-B`/`--test-dir` overrides if you would rather keep it (it is gitignored either way).

## What is covered

Three suites, one binary each so that a crash under ASan still lets the others report:

- **`test_ownership`** — the JSValue lifetime rules from `jsvm_core.cpp`. The self-stopping timer and the stale widget handle, in both their write and read forms; the two different release paths for event bindings (the `LV_EVENT_DELETE` hook for a child, `jsvm_stop()`'s sweep for the screen object itself); timers left running at teardown; and a double `.stop()`, which is a double free if `timer_release()` stops nulling the wrapper's opaque.
- **`test_props`** — `apply_props` and the string→enum tables from `bindings_lv.cpp`. The one that earns its place is the ordering rule: `value` is applied twice, because it is read before `options` exists and LVGL clamps a selection against an empty list to zero, so `{options, value}` in a single call would otherwise always land on item 0. Also colours in all three accepted forms, percentage/pixel/content sizes, range clamping, unknown props degrading rather than throwing, and the widget-kind guard in both directions.
- **`test_reload`** — start and stop the same script 25 times and assert the heap returns exactly to where it began, in both bytes and block count. This is the test the issue asked for first, and the reason is procedural: reload correctness was checked by hand, five cycles at a time, by watching a serial log. It also covers teardown after a script that throws half-way, and `jsvm_start()` called repeatedly with no intervening `jsvm_stop()`, which is what `sys.launch()` ends up doing.

Two independent leak checks run over all of it. ASan's own detector catches what is unreachable at exit; `host_heap_live_bytes()` catches what is still reachable but should not be — a binding left on `g_events`, a `JS_DupValue` never released. Neither finds the other's bugs, so both are asserted.

## What it does not cover, and will not

Nothing here says anything about hardware. The panel, touch, PSRAM capabilities, SPI DMA and FreeRTOS task behaviour are all absent by construction, and `app/selftest.js` on a real board remains the acceptance test. Specifically:

- **The flush path is a no-op.** The byte-swap invariant (`lv_draw_sw_rgb565_swap()` paired with `draw16bitBeRGBBitmap()`) is display-side and cannot be tested here. Rendering runs; the pixels are discarded.
- **`heap_caps_*` maps onto `malloc`, and the capability flags are ignored.** That is the point — it puts every allocation where ASan can see it — but it means a host run cannot tell you whether something would have fitted in real PSRAM or in DMA-capable internal RAM. The free-size numbers the layer prints are synthetic. Never read them as a memory measurement.
- **There is no input device.** Without one `lv_indev_active()` is always null, so the event trampoline only ever takes its `fn(widget)` path and never `fn(widget, x, y)`. Synthesizing touch would mean inventing coordinates the panel never produced, so event coverage stops at registration and argument validation, exactly as it does in `app/selftest.js`.
- **Timing is virtual.** `millis()` and LVGL's tick both come from a counter the tests step explicitly, which makes "this timer fired twice" exact instead of racing wall-clock time. It is not a claim about scheduling on the device.

## Two deliberate scope decisions

**`jsvm_app.cpp` is not compiled.** The supervisor is written against Arduino `String`, `Serial.available()`/`read()` and `File.print()`. Stubbing those is a larger surface than every other stub combined, and it is the surface most likely to rot. The issue scoped the first pass to `jsvm_core.cpp` plus `bindings_lv.cpp` for that reason. The path resolution it also lists (`loadScript`/`resolveFs`, the `flash:` prefix, the card-then-flash fallback) is the obvious next addition, and the in-memory `fs::FS` stub already exists to support it — what is missing is only an Arduino `String` and a line-reader on the Serial stub.

**Wi-Fi is compiled out (`JSVM_WITH_WIFI=0`).** `WiFi.h`, `HTTPClient.h` and `NetworkClientSecure.h` are the largest stub surface in the layer and the least meaningful without a network. `sys` and `fs` *are* compiled in, because their stubs are small and genuinely behave: the filesystem is a real in-memory one, and `Preferences` is a real key-value store that outlives a `jsvm_stop()`/`jsvm_start()` pair the way NVS outlives a reboot.

A consequence worth stating: **`app/selftest.js` cannot currently run as the host test body**, which the issue floated as the cheapest way to get coverage. It makes five `wifi.status()` calls and eighteen `sys.*`/`fs.*` calls, so with Wi-Fi compiled out it would fail on the binding surface rather than on behaviour. Compiling Wi-Fi in — or teaching the host `wifi` module to report a permanent "no radio" — is what would unlock sharing that one file between both targets. It remains the right end state, and it is not free.

## The stub layer, and how it rots

`stubs/` is the cost centre. Every `#include <Arduino.h>`-ism the C layer grows later needs something here, and a stub that drifts from the real API is worse than no stub because it compiles.

Two things keep it honest. The stubs mirror the real include graph rather than being a convenience header — `Arduino.h` pulls in `esp_heap_caps.h` because the ESP32 core's does, and `bindings_fs.cpp` relies on exactly that. And the host `lv_conf.h` **includes the board's copy** instead of duplicating it, overriding only the three settings the host needs differently, so which widgets exist and which fonts a script may select are physically the same values the panel builds with. A host test cannot pass against a configuration the board does not run.

The override that matters is `LV_USE_STDLIB_MALLOC` → `LV_STDLIB_CLIB`. LVGL's builtin allocator is a pool it manages itself, so ASan would see one large `malloc` and none of the widget allocations inside it; routing LVGL onto the C library's `malloc` is what makes a widget-level use-after-free visible.

## In CI

The `host-test` job in [`.github/workflows/ci.yml`](../.github/workflows/ci.yml) runs on every push. It is not currently one of the checks the ruleset requires to merge — those are `scripts`, `firmware` and `review` (see [`pr-automation.md`](pr-automation.md)) — so adding it to the required set is a separate, manual repository-settings change.
