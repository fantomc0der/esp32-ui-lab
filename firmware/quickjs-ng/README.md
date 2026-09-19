# QuickJS-ng, vendored as an Arduino library

[QuickJS-ng](https://github.com/quickjs-ng/quickjs) **v0.17.0** (commit `6d46d07d04041b40f4f49eaa7fdebe44c314c699`), packaged so every board sketch can share one copy. Contents of `src/`: the four core sources from upstream's CMake `qjs_sources` (`quickjs.c`, `libregexp.c`, `libunicode.c`, `dtoa.c`) plus every header they include, 18 files. `quickjs-libc.c` (upstream's POSIX stdlib layer) is deliberately **not** vendored: it needs processes/fds/sockets, and the sketches provide their own bindings instead. License: MIT, see `LICENSE`. That version-and-commit line is the pin: `tools/vendor-quickjs.ps1` reads the baseline from it and rewrites it, so it is the one place the provenance is stated.

## Local modifications

None to the source: `src/` is byte-identical to upstream at the pinned commit. One upstream Xtensa miscompile is worked around with a build flag instead, `-fno-strict-aliasing`, described below. Through v0.15.1 this folder carried a local patch for five Xtensa `int32_t` pointer-type mismatches, which upstream fixed in v0.17.0; the history is in [`docs/engine-notes.md`](../../docs/engine-notes.md).

## Re-vendoring

```powershell
.\tools\vendor-quickjs.ps1 -Target v0.18.0
```

That clones upstream into `.temp/`, checks out the target, copies an explicit 18-file manifest into `src/`, and rewrites the pin here and in `library.properties`.

Passing the currently pinned SHA is the reproducibility check: the tree must come back byte-identical, so `git diff` is empty. The manifest is an explicit list, not a glob, so that a future upstream reorganisation cannot silently pull `quickjs-libc.c` back in. If upstream adds a core source, the script errors on the missing name or the sketch fails to link, which is the loud failure this trades for.

## How sketches use it

This folder is not installed into the Arduino libraries directory; pass it explicitly:

```powershell
arduino-cli compile --library .\firmware\quickjs-ng --library .\firmware\lvgl-js-bindings -b <FQBN> .\firmware\boards\<name>
```

Three build requirements, all supplied by each sketch's `build_opt.h` (the esp32 core picks that file up automatically and applies its flags to library sources too): `-D_GNU_SOURCE` (upstream's CMake adds it), `-DNDEBUG` (strips QuickJS's debug dump machinery, worth ~90 KB of flash), and `-fno-strict-aliasing` (on Xtensa, where `uint32_t` is `unsigned long`, strict aliasing miscompiles the string iterator so it never advances; see [`docs/engine-notes.md`](../../docs/engine-notes.md), Trap 5).

Allocator rule for every consumer: report `js_malloc_usable_size` as 0. QuickJS treats the reported value as writable capacity, and with IDF heap poisoning enabled `heap_caps_get_allocated_size()` counts the tail canary in it, so reporting real sizes corrupts the heap (found the hard way on hardware; see [`docs/engine-notes.md`](../../docs/engine-notes.md)).
