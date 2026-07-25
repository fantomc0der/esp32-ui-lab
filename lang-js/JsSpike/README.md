# JsSpike — Phase 1 engine spike

Minimal go/no-go sketch proving [QuickJS-ng](https://github.com/quickjs-ng/quickjs) runs on the Waveshare ESP32-S3-Touch-LCD-1.47 with its JS heap in PSRAM. Serial only, no display. The plan this gates: [`docs/lang-js/js-scripting-plan.md`](../../docs/lang-js/js-scripting-plan.md).

## Where the engine lives

The QuickJS-ng sources are vendored once for all `lang-js/` sketches as an Arduino library in [`../quickjs-ng/`](../quickjs-ng/README.md) (version, exclusions, and the five Xtensa type patches are documented there). This folder holds only the spike sketch itself plus `build_opt.h`, which supplies the flags the engine needs (`-D_GNU_SOURCE`, `-DNDEBUG`).

## Build & flash

Same FQBN as the C demo (see [`docs/lang-c/build-and-flash.md`](../../docs/lang-c/build-and-flash.md)), plus the `--library` flag pointing at the shared engine:

```powershell
arduino-cli compile --library ..\quickjs-ng -b esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=cdc,FlashMode=qio,USBMode=hwcdc .\JsSpike
arduino-cli upload  -b <same FQBN> -p COM4 .\JsSpike
```

## Measured results (hardware, 2026-07-25)

Phase 1 exit criteria, all met:

| Metric | Value |
|---|---|
| Flash cost of the engine | **429,248 bytes** (731,316 sketch − 302,068 bare-Serial baseline, `-DNDEBUG`) |
| Static RAM cost | 216 bytes (23,024 − 22,808) |
| Internal RAM at runtime | **348 bytes** for runtime + context (316,084 → 315,736 free) |
| PSRAM at runtime | 79,672 bytes for runtime + context; 20k-object churn peaked at ~3.26 MB, fully reclaimed by `JS_RunGC` |
| Eval times | `1+1` 1.1 ms; closures 2.8 ms; JSON round-trip 2.9 ms; 20k-object alloc loop 1.31 s; REPL 1000-element reduce 10.6 ms |

The one runtime bug found on hardware: reporting `heap_caps_get_allocated_size()` as `js_malloc_usable_size` corrupts the heap, because QuickJS writes up to the reported size while IDF heap poisoning counts its tail canary in it. The allocator now reports 0 ("unknown"), the upstream default for platforms without `malloc_usable_size`.

Serial-capture gotcha: opening COM4 with DTR+RTS both asserted can drop the S3 into the ROM bootloader (banner `ESP-ROM:esp32s3-20210327` and nothing else). Recover with an esptool-style pulse: open with both deasserted, pulse RTS ~100 ms, then assert DTR to read.

## What it prints

Boot runs an eval suite (arithmetic, closures, a 20k-object GC churn, JSON, a native `print()` binding) with per-eval timings and free-memory snapshots that prove the JS heap lands in PSRAM, not internal RAM. After boot, the sketch is a one-line serial REPL: type JavaScript at 115200 baud, get the result back.
