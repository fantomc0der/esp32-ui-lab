# Docs index

Topic notes for this repo — written so future-you (or a future session) can recall *why* things are the way they are without re-deriving them.

Organized by topic, roughly following the repo layout (see the [top-level README](../README.md)).

## Orientation

| Doc | What's in it |
|---|---|
| [architecture.md](architecture.md) | How the repo fits together: the platform/board seam, the shared display and touch stack, the single-task rule, and the constraints that shaped everything |

## The platform

| Doc | What's in it |
|---|---|
| [binding-api.md](binding-api.md) | The JavaScript API exposed to apps (lv/sys/fs/wifi/fetch/console), props reference, responsive layout guidance, and the GC ownership rules |
| [runtime-architecture.md](runtime-architecture.md) | How the runtime works internally: the layer stack, threading rule, call and event flow, JSValue ownership, teardown order, memory map, and how to extend it |
| [portability.md](portability.md) | What the runtime actually requires (PSRAM, flash, LVGL 9), which ESP32 variants qualify, why screen size is not a constraint, and the three-hook porting contract |
| [build-and-deploy.md](build-and-deploy.md) | Building the firmware (the --library flag), deploying app.js via SD card or serial, the REPL/upload serial commands, expected boot log |
| [design-rationale.md](design-rationale.md) | Why this exists rather than lvgljs, what was kept from it, estimated vs actual cost, and how each predicted risk turned out |
| [engine-notes.md](engine-notes.md) | QuickJS-ng on this board: spike measurements, the heap-poisoning/usable_size trap, the job pump, the Xtensa type patches, the DTR/RTS bootloader trap |

## The hardware

Applies to every board sketch, and to the frozen C dashboard, since they drive the panel identically.

| Doc | What's in it |
|---|---|
| [hardware/README.md](hardware/README.md) | The board itself: pinout, chip quirks, what the microSD slot is for, traps specific to this exact Waveshare variant |
| [hardware/display-pipeline.md](hardware/display-pipeline.md) | How pixels get from LVGL to the JD9853 panel: driver workaround, byte order, buffers, rotation |
| [hardware/touch.md](hardware/touch.md) | The AXS5106L driver, the I2C protocol, and the full story of the axis-mapping bug — found and fixed with hardware measurement |

## The frozen C dashboard

Kept because it is where the hardware was first proven; see [`experiments/c-dashboard/`](../experiments/c-dashboard/README.md).

| Doc | What's in it |
|---|---|
| [experiments/c-dashboard/portability.md](experiments/c-dashboard/portability.md) | Which code in the C dashboard is specific to this device vs. reusable elsewhere, file by file |
| [experiments/c-dashboard/build-and-flash.md](experiments/c-dashboard/build-and-flash.md) | Arduino board settings, the FQBN, flashing from the CLI, scripted serial capture on Windows |

Start with [`BUILDING.md`](../BUILDING.md) at the repo root for setup and deployment; come here for the deeper "how does this actually work" material.

Authoring convention for everything here: prose is never hard-wrapped, one continuous line per paragraph or list item, so it reflows to the reader's window. Line breaks are for structure (list items, paragraphs, code blocks, table rows), not for width.
