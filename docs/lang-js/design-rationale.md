# Why the JS runtime is built this way

The design decisions behind [`lang-js/`](../../lang-js/README.md), the alternative that was rejected, and how the original estimates and risks actually turned out. For how the result works, see [`architecture.md`](architecture.md); for what scripts can call, [`binding-api.md`](binding-api.md).

Goal, in one sentence: write UI logic for this board in JavaScript, edited without recompiling firmware.

## Why not use lvgljs

The obvious move is to use LVGL's own JavaScript binding, [lv_binding_js / lvgljs](https://github.com/lvgl/lv_binding_js). It doesn't fit this class of hardware, and the mismatch is structural rather than a matter of effort.

lvgljs is QuickJS wrapped in **txiki.js**, plus **libuv** and **curl**, plus a React/JSX virtual-DOM layer. Each of those middle layers assumes an operating system this board doesn't have. libuv is a POSIX abstraction over epoll/kqueue, file descriptors, processes and pthreads. curl wants BSD sockets and a TLS stack. txiki.js supplies a POSIX-shaped standard library (filesystem, process spawning) on top of them. Under FreeRTOS on an ESP32-S3, none of that exists in the form they need.

Its own build documentation confirms the target (verified against upstream on 2026-07-25): two builds exist, a simulator and a "device" build, and the device build means **embedded Linux**. It is compiled against a Yocto SDK sysroot and renders through a framebuffer device, DRM, or Wayland, with a default screen size of 1024×600. There is no ESP-IDF, FreeRTOS, or bare-metal target.

The display layer mismatches from the other end too. Their HAL writes to `/dev/fb0` or DRM; this panel is a JD9853 on SPI fed by an LVGL flush callback with a manual RGB565 byte swap.

Scale compounds it. Their device target is Raspberry Pi class: hundreds of megabytes of RAM behind an MMU, driving 1024×600. This board has 512 KB of internal SRAM plus 8 MB of PSRAM and a 3 MB app partition, driving 320×172.

So porting would mean replacing the event loop, replacing the networking stack, stripping the POSIX standard library, swapping the display HAL, and then fitting a React reconciler into flash. Everything except the idea gets rewritten, and the idea is the valuable part.

**What would change this:** if lvgljs ever grows an ESP-IDF target, the comparison is worth redoing from scratch.

## What was kept instead

The architecture, which is the part that transfers: **a JS engine compiled into the firmware, a hand-written binding layer over LVGL's C API, and scripts stored as data rather than code.**

Even the engine choice is shared, since QuickJS-ng is the maintained fork of the same QuickJS that sits underneath txiki.js. What was dropped is the scaffolding around it. The cost of that scaffolding, in features, is real and worth stating plainly: no JSX, no React component model, no CSS-like styling, no animation or image handling, no npm or bundler workflow.

## Budget: estimated versus actual

The feasibility estimates made before any code was written, against what it actually cost:

| Resource | Estimated | Actual |
|---|---|---|
| Flash for the engine | 500–800 KB | **429 KB** (`-DNDEBUG`, measured against a bare-Serial baseline) |
| JS heap | 256 KB – 1 MB, in PSRAM | **~80 KB at rest**; a 20k-object stress peaked at 3.26 MB and was fully reclaimed by GC |
| Task stack | 16–32 KB | 32 KB `loopTask`, with the VM capped at 20 KB so it hits its own guard first |
| CPU | "fine at UI event rates" | `1+1` in 1.1 ms; the whole 4-tab UI builds in 74 ms; rendering is untouched |
| Whole firmware | not estimated | 1.79 MB, 56% of the 3 MB app partition |

Every estimate was conservative, which is the desirable direction. The engine landed about 14% under the low end of the flash estimate, and roughly 46% under the high end.

## How it was built

Spike first, with an explicit go/no-go gate. Before a single binding was written, a throwaway serial-only sketch vendored the engine, ran it on hardware, routed its heap to PSRAM, and measured the cost, against pre-committed exit criteria (eval works on hardware, flash under 1 MB, heap demonstrably in PSRAM). Only after those passed did the binding layer, the loader, the reload path, and the proof app follow.

That ordering was the single most useful decision. The one question that could have killed the project (does a full JS engine fit and run here at all?) got answered in isolation, cheaply, before anything depended on the answer. The spike sketch itself was deleted once the runtime superseded it; its measurements and the traps it exposed live in [`engine-notes.md`](engine-notes.md).

## Risks, and how they turned out

| Predicted risk | Outcome |
|---|---|
| QuickJS build friction on Xtensa | **Mild.** Five one-line type fixes where `int` locals meet `int32_t*` parameters, because this toolchain types `int32_t` as `long`. The JerryScript escape hatch was never needed. |
| `JSValue` lifetime bugs | **The real risk, and it stayed the hardest part.** Handled by the `LV_EVENT_DELETE` hook plus a global registry for screen-level bindings; five consecutive reload cycles leaked no internal RAM. One hazard emerged later than the design: `.clean()` made widget handles capable of dangling within a single run. Documented in [`architecture.md`](architecture.md), not yet hardened. |
| Scope creep | **Held, then grew deliberately.** v1 shipped at roughly 15 functions. Recreating the C demo added tabview, chart, `.bounds()`, `.clean()`, `.push()`, and `.addTab()`, taking it to about 20. The growth was driven by a concrete parity goal rather than speculation, which is the distinction worth keeping. |
| PSRAM latency for the JS heap | **Non-issue.** JavaScript runs at event rate, not pixel rate, exactly as predicted. |

One risk was not predicted and should have been. QuickJS's pending-job queue is normally drained by quickjs-libc's event loop, which isn't vendored here, so promises and `async`/`await` queued silently and never ran while everything else worked. The lesson generalizes: when you vendor a runtime's core and drop its host layer, enumerate what that host layer was doing for you.

## What this is not

Not an lvgljs port, not React, not npm. It is "MicroPython-style scripting, but JavaScript, for this board": the 20% of lvgljs that delivers 80% of the point, which is UI logic as editable data.
