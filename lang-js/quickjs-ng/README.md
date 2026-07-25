# QuickJS-ng, vendored as an Arduino library

[QuickJS-ng](https://github.com/quickjs-ng/quickjs) **v0.15.1** (tag `fd0a0210`), packaged so multiple sketches in `lang-js/` can share one copy. Contents of `src/`: the four core sources from upstream's CMake `qjs_sources` (`quickjs.c`, `libregexp.c`, `libunicode.c`, `dtoa.c`) plus every header they include. `quickjs-libc.c` (upstream's POSIX stdlib layer) is deliberately **not** vendored: it needs processes/fds/sockets, and the sketches provide their own bindings instead. License: MIT, see `LICENSE`.

## Local modifications

Five one-line type fixes in `quickjs.c` where an `int` local is passed to a function expecting `int32_t*` (or vice versa): on the Xtensa toolchain `int32_t` is `long int`, not `int`, so GCC 14 rejects the mismatched pointers. Each site carries an `xtensa` comment; grep `int32_t is long` to find them when re-vendoring a newer release.

## How sketches use it

This folder is not installed into the Arduino libraries directory; pass it explicitly:

```powershell
arduino-cli compile --library .\quickjs-ng -b <FQBN> .\JsHost
```

Two build requirements, both supplied by each sketch's `build_opt.h` (the esp32 core picks that file up automatically and applies its flags to library sources too): `-D_GNU_SOURCE` (upstream's CMake adds it) and `-DNDEBUG` (strips QuickJS's debug dump machinery, worth ~90 KB of flash).

Allocator rule for every consumer: report `js_malloc_usable_size` as 0. QuickJS treats the reported value as writable capacity, and with IDF heap poisoning enabled `heap_caps_get_allocated_size()` counts the tail canary in it, so reporting real sizes corrupts the heap (found the hard way on hardware; see [`docs/lang-js/engine-notes.md`](../../docs/lang-js/engine-notes.md)).
