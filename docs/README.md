# Docs index

Topic notes for this repo — written so future-you (or a future session) can recall *why* things are the way they are without re-deriving them.

Organized to mirror the repo's per-language `lang-*/` directories (see the [top-level README](../README.md)).

## Shared (applies to both)

| Doc | What's in it |
|---|---|
| [architecture.md](architecture.md) | How the repo fits together: the stack C and JavaScript share, where they diverge, the single-task rule, and the constraints that shaped everything |
| [hardware.md](hardware.md) | The board itself: pinout, chip quirks, what the microSD slot is for, traps specific to this exact Waveshare variant |

## `lang-c/` — C/C++

| Doc | What's in it |
|---|---|
| [lang-c/display-pipeline.md](lang-c/display-pipeline.md) | How pixels get from LVGL to the JD9853 panel: driver workaround, byte order, buffers, rotation |
| [lang-c/touch.md](lang-c/touch.md) | The AXS5106L driver, the I2C protocol, and the full story of the axis-mapping bug — found and fixed with hardware measurement |
| [lang-c/portability.md](lang-c/portability.md) | Which code is specific to this device vs. reusable elsewhere, file by file |
| [lang-c/build-and-flash.md](lang-c/build-and-flash.md) | Arduino board settings, the FQBN, flashing from the CLI, scripted serial capture on Windows |

## `lang-js/` — JavaScript

| Doc | What's in it |
|---|---|
| [lang-js/architecture.md](lang-js/architecture.md) | How the runtime works internally: the layer stack, threading rule, call and event flow, JSValue ownership, teardown order, memory map, and how to extend it |
| [lang-js/design-rationale.md](lang-js/design-rationale.md) | Why this exists rather than lvgljs, what was kept from it, estimated vs actual cost, and how each predicted risk turned out |
| [lang-js/build-and-deploy.md](lang-js/build-and-deploy.md) | Building js-host (the --library flag), deploying app.js via SD card or serial, the REPL/upload serial commands, expected boot log |
| [lang-js/binding-api.md](lang-js/binding-api.md) | The JavaScript API exposed to app.js (lv/sys/wifi/console), props reference, and the GC ownership rules |
| [lang-js/portability.md](lang-js/portability.md) | What the runtime actually requires (PSRAM, flash, LVGL 9), which ESP32 variants qualify, why screen size is not a constraint, and the three-hook porting contract |
| [lang-js/engine-notes.md](lang-js/engine-notes.md) | QuickJS-ng on this board: spike measurements, the heap-poisoning/usable_size trap, the job pump, the Xtensa type patches, the DTR/RTS bootloader trap |

Start with [`BUILDING.md`](../BUILDING.md) at the repo root for setup + deployment of either demo; come here for the deeper "how does this actually work" material.

Authoring convention for everything here: prose is never hard-wrapped, one continuous line per paragraph or list item, so it reflows to the reader's window. Line breaks are for structure (list items, paragraphs, code blocks, table rows), not for width.
