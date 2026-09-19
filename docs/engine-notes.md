# QuickJS-ng on the ESP32-S3: measurements and traps

Engine-level knowledge collected while bringing QuickJS-ng up on this board. The vendored engine itself (version, how sketches link it) is documented next to the code in [`firmware/quickjs-ng/README.md`](../firmware/quickjs-ng/README.md); this page records what was measured and what bit us, so nobody has to rediscover it.

## Phase 1 spike measurements (hardware, 2026-07-25)

Measured with a serial-only spike sketch (engine + REPL, no display; it lived at `lang-js/JsSpike/`, deleted after the board sketch superseded it — commit `34e0a13` has it in full). Baseline for the deltas: a bare `Serial.begin` sketch on the same FQBN.

| Metric | Value |
|---|---|
| Flash cost of the engine | **429,248 bytes** (731,316 sketch − 302,068 baseline, `-DNDEBUG`) |
| Static RAM cost | 216 bytes |
| Internal RAM at runtime | **348 bytes** for runtime + context (JS heap entirely in PSRAM) |
| PSRAM at runtime | 79,672 bytes for runtime + context; a 20k-object churn peaked at ~3.26 MB, fully reclaimed by `JS_RunGC` |
| Eval times | `1+1` 1.1 ms; closures 2.8 ms; JSON round-trip 2.9 ms; 20k-object alloc loop 1.31 s; 1000-element reduce 10.6 ms |

The pre-committed exit criteria (eval on hardware, < 1 MB flash, heap demonstrably in PSRAM) were all met with room to spare, which is why there is no JerryScript fallback in this repo. Why the spike existed at all: [`design-rationale.md`](design-rationale.md).

## Trap 1: `js_malloc_usable_size` must report 0

QuickJS treats the reported usable size as *writable capacity* and fills it to the byte (string builders grow into the slack). With IDF heap poisoning enabled, `heap_caps_get_allocated_size()` counts the tail-canary region in its answer, so reporting it lets JS string code overwrite the canary — the first `join()` aborts the chip with `CORRUPT HEAP: Bad tail`. Report 0 ("unknown"), which is upstream's own default for platforms without `malloc_usable_size`.

## Trap 2: promises need a job pump

`JS_ExecutePendingJob` is normally driven by quickjs-libc's event loop, which is not vendored (it needs POSIX). Without pumping it yourself, `.then()` callbacks and `async`/`await` continuations queue forever and never run — everything else works, which makes it easy to miss. The firmware pumps the queue once per `loop()` (`jsvm_pump()`).

## Trap 3: the Xtensa `int32_t` type mismatch (compile errors fixed upstream in v0.17.0; see Trap 5)

This toolchain typedefs `int32_t` as `long int`, not `int`. They're the same width, but GCC 14 hard-errors on mixed `int*`/`int32_t*` arguments, and QuickJS-ng up to v0.16.x tripped that in five places (`find_line_num`, `js_parseInt`, `remainingElementsCount_add`, `js_promise_all_resolve_element`, `js_atomics_notify`), each a local whose type did not match the pointer its callee takes. Through v0.15.1 the vendored copy carried five one-line local-variable type fixes as a local patch, which `tools/vendor-quickjs.ps1` replayed onto each new upstream with a rebase.

Upstream made the same five changes in [quickjs-ng#1657](https://github.com/quickjs-ng/quickjs/pull/1657) (commit `d8e1cc6`, fixing issue #1624), first released in v0.17.0. From that version on the vendored engine is unmodified upstream, and the patch and its replay machinery are gone. A later upstream that reintroduces a plain mismatch fails the sketch build loudly, and `xtensa-esp32s3-elf-gcc -fsyntax-only -std=gnu17 -D_GNU_SOURCE -I. quickjs.c` surfaces those sites in seconds without a full sketch build. It does not catch a mismatch hidden behind an explicit pointer cast, which compiles cleanly and is miscompiled instead; one such site remains upstream, neutralised by a build flag as described in Trap 5.

## Trap 4: DTR/RTS can trap the board in the ROM bootloader

Opening the native-USB COM port with DTR and RTS both asserted can reset the S3 into the ROM bootloader — the only serial output is `ESP-ROM:esp32s3-20210327` and the sketch never runs. Recover with an esptool-style sequence: open with both deasserted, pulse RTS high for ~100 ms, drop it, *then* assert DTR (needed for CDC to transmit) and read.

## Trap 5: string iteration never advanced (worked around with `-fno-strict-aliasing`)

Without the workaround, `for (const c of str)`, `[...str]` and `Array.from(str)` return the first BMP character forever, so the loop either runs the heap out (`InternalError: out of memory`) or, if it drops each value, spins until the board is reset. `"ab"[Symbol.iterator]()` shows it directly: a second `.next()` returns `"a"` again. Both v0.15.1 and v0.17.0 did this on the board, while upstream v0.17.0 built for x86 (64- and 32-bit, `-O2` and `-Os`) is correct.

The cause is the Trap 3 root cause wearing a cast. `js_string_iterator_next` declares `uint32_t idx` and calls `string_getc(p, (int *)&idx)`. On Xtensa `uint32_t` is `unsigned long`, which may not alias `int`, so under strict aliasing GCC decides the call cannot change `idx` and deletes the following `it->idx = idx` as a redundant store; the explicit cast suppresses the diagnostic that caught the other five sites. The `-S` output for the function shows no store to `it->idx` after `call8 string_getc`, and it reappears with `-fno-strict-aliasing`.

The board sketch's `build_opt.h` therefore passes `-fno-strict-aliasing`, which keeps the vendored engine unmodified. The esp32 core has no per-library flags, so it applies to LVGL and the bindings too; it cost 1,804 bytes of flash (0.09%) and no measurable change in eval times. The host build passes it to the engine as well, so both targets compile QuickJS the same way. When checking the flag on hardware, rebuild with `arduino-cli compile --clean`: a changed `build_opt.h` does not reliably invalidate cached library objects, and a first attempt here appeared to show the flag failing for exactly that reason. [`app/engine-probe.js`](../app/engine-probe.js) covers it as `string iterator advances (Trap 5)`, which fails in constant time rather than by exhausting the heap; the whole probe passes 49 of 49, none skipped, with the flag. CI's `scripts` job fails any board whose `build_opt.h` lacks the flag, since a sketch without it builds fine and only hangs at run time.

The real fix is declaring `idx` as `int` upstream, a one-line change in the style of quickjs-ng#1657. Once a release carries it, the flag can go, but only after re-running the probe on the board, since it also guards against any other cast-hidden mismatch nobody has found yet.
