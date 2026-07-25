# QuickJS-ng on the ESP32-S3: measurements and traps

Engine-level knowledge collected while bringing QuickJS-ng up on this board. The vendored engine itself (version, patches, how sketches link it) is documented next to the code in [`lang-js/quickjs-ng/README.md`](../../lang-js/quickjs-ng/README.md); this page records what was measured and what bit us, so nobody has to rediscover it.

## Phase 1 spike measurements (hardware, 2026-07-25)

Measured with a serial-only spike sketch (engine + REPL, no display; it lived at `lang-js/JsSpike/`, deleted after JsHost superseded it — commit `34e0a13` has it in full). Baseline for the deltas: a bare `Serial.begin` sketch on the same FQBN.

| Metric | Value |
|---|---|
| Flash cost of the engine | **429,248 bytes** (731,316 sketch − 302,068 baseline, `-DNDEBUG`) |
| Static RAM cost | 216 bytes |
| Internal RAM at runtime | **348 bytes** for runtime + context (JS heap entirely in PSRAM) |
| PSRAM at runtime | 79,672 bytes for runtime + context; a 20k-object churn peaked at ~3.26 MB, fully reclaimed by `JS_RunGC` |
| Eval times | `1+1` 1.1 ms; closures 2.8 ms; JSON round-trip 2.9 ms; 20k-object alloc loop 1.31 s; 1000-element reduce 10.6 ms |

The Phase 1 exit criteria from the [plan](js-scripting-plan.md) (eval on hardware, < 1 MB flash, heap demonstrably in PSRAM) were all met with room to spare, which is why there is no JerryScript fallback in this repo.

## Trap 1: `js_malloc_usable_size` must report 0

QuickJS treats the reported usable size as *writable capacity* and fills it to the byte (string builders grow into the slack). With IDF heap poisoning enabled, `heap_caps_get_allocated_size()` counts the tail-canary region in its answer, so reporting it lets JS string code overwrite the canary — the first `join()` aborts the chip with `CORRUPT HEAP: Bad tail`. Report 0 ("unknown"), which is upstream's own default for platforms without `malloc_usable_size`.

## Trap 2: promises need a job pump

`JS_ExecutePendingJob` is normally driven by quickjs-libc's event loop, which is not vendored (it needs POSIX). Without pumping it yourself, `.then()` callbacks and `async`/`await` continuations queue forever and never run — everything else works, which makes it easy to miss. JsHost pumps the queue once per `loop()` (`jsvm_pump()`).

## Trap 3: the Xtensa `int32_t` type mismatch

This toolchain typedefs `int32_t` as `long int`, not `int`. They're the same width, but GCC 14 hard-errors on mixed `int*`/`int32_t*` arguments, which upstream QuickJS-ng trips in five places. The vendored copy carries five one-line local-variable type fixes, each marked with an `xtensa` comment (grep `int32_t is long`). Re-check after re-vendoring: `xtensa-esp32s3-elf-gcc -fsyntax-only -std=gnu17 -D_GNU_SOURCE -I. quickjs.c` surfaces all of them in seconds without a full sketch build.

## Trap 4: DTR/RTS can trap the board in the ROM bootloader

Opening the native-USB COM port with DTR and RTS both asserted can reset the S3 into the ROM bootloader — the only serial output is `ESP-ROM:esp32s3-20210327` and the sketch never runs. Recover with an esptool-style sequence: open with both deasserted, pulse RTS high for ~100 ms, drop it, *then* assert DTR (needed for CDC to transmit) and read.
