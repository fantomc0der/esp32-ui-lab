# QuickJS-ng, vendored as an Arduino library

[QuickJS-ng](https://github.com/quickjs-ng/quickjs) **v0.15.1** (commit `fd0a0210b7be00957751871e7e01b8291268fc29`), packaged so every board sketch can share one copy. Contents of `src/`: the four core sources from upstream's CMake `qjs_sources` (`quickjs.c`, `libregexp.c`, `libunicode.c`, `dtoa.c`) plus every header they include, 18 files. `quickjs-libc.c` (upstream's POSIX stdlib layer) is deliberately **not** vendored: it needs processes/fds/sockets, and the sketches provide their own bindings instead. License: MIT, see `LICENSE`. That version-and-commit line is the pin: `tools/vendor-quickjs.ps1` reads the baseline from it and rewrites it, so it is the one place the provenance is stated.

## Local modifications

Everything in `src/` is upstream except one patch, kept in `patches/` rather than only inline: five type fixes in `quickjs.c` where a local's type does not match the pointer its callee takes. On the Xtensa toolchain `int32_t` is `long int` rather than `int`, so four `int` locals passed to functions taking `int32_t*` (and one `int32_t` local passed to a function taking `int*`) are incompatible-pointer errors under GCC 14. The sites still carry an `xtensa` comment, but `patches/0001-xtensa-int32-pointer-types.patch` is the authoritative record of the delta, in `git format-patch` form so it is `git am`-able and can go upstream as-is.

`src/` holds the patched, working code; the patch is what regenerates it from pristine upstream, not a pending change.

## Re-vendoring

```powershell
.\tools\vendor-quickjs.ps1 -Target v0.16.0
```

That clones upstream into `.temp/`, checks out the pinned baseline, replays `patches/` onto it with `git am`, rebases onto the target, regenerates `patches/` against the new baseline so fuzz does not accumulate, copies an explicit 18-file manifest into `src/`, and rewrites the pin here and in `library.properties`. Because it rebases rather than `git apply`s, upstream moving the patched code produces real conflict markers instead of a flat rejection; re-run with `-KeepWorkspace` to inspect them.

Passing the currently pinned SHA is the reproducibility check: the tree must come back byte-identical, so `git diff` is empty. The manifest is an explicit list, not a glob, so that a future upstream reorganisation cannot silently pull `quickjs-libc.c` back in. If upstream adds a core source, the script errors on the missing name or the sketch fails to link, which is the loud failure this trades for.

## How sketches use it

This folder is not installed into the Arduino libraries directory; pass it explicitly:

```powershell
arduino-cli compile --library .\firmware\quickjs-ng --library .\firmware\lvgl-js-bindings -b <FQBN> .\firmware\boards\<name>
```

Two build requirements, both supplied by each sketch's `build_opt.h` (the esp32 core picks that file up automatically and applies its flags to library sources too): `-D_GNU_SOURCE` (upstream's CMake adds it) and `-DNDEBUG` (strips QuickJS's debug dump machinery, worth ~90 KB of flash).

Allocator rule for every consumer: report `js_malloc_usable_size` as 0. QuickJS treats the reported value as writable capacity, and with IDF heap poisoning enabled `heap_caps_get_allocated_size()` counts the tail canary in it, so reporting real sizes corrupts the heap (found the hard way on hardware; see [`docs/engine-notes.md`](../../docs/engine-notes.md)).
