# Docs index

Topic notes for this repo — written so future-you (or a future session) can recall *why* things are the way they are without re-deriving them.

Organized to mirror the repo's per-language `lang-*/` directories (see the [top-level README](../README.md)).

## Shared (applies to every way)

| Doc | What's in it |
|---|---|
| [hardware.md](hardware.md) | The board itself: pinout, chip quirks, what the microSD slot is for, traps specific to this exact Waveshare variant |

## `lang-c/` — the C/C++ way

| Doc | What's in it |
|---|---|
| [lang-c/display-pipeline.md](lang-c/display-pipeline.md) | How pixels get from LVGL to the JD9853 panel: driver workaround, byte order, buffers, rotation |
| [lang-c/touch.md](lang-c/touch.md) | The AXS5106L driver, the I2C protocol, and the full story of the axis-mapping bug — found and fixed with hardware measurement |
| [lang-c/portability.md](lang-c/portability.md) | Which code is specific to this device vs. reusable elsewhere, file by file |
| [lang-c/build-and-flash.md](lang-c/build-and-flash.md) | Arduino board settings, the FQBN, flashing from the CLI, scripted serial capture on Windows |

## `lang-js/` — the JavaScript way

| Doc | What's in it |
|---|---|
| [lang-js/js-scripting-plan.md](lang-js/js-scripting-plan.md) | The phased plan for running QuickJS + LVGL bindings on this board, with feasibility budget and risks — all phases now done and hardware-verified |
| [lang-js/binding-api.md](lang-js/binding-api.md) | The JavaScript API exposed to app.js (lv/sys/wifi/console), props reference, and the GC ownership rules |
| [lang-js/engine-notes.md](lang-js/engine-notes.md) | QuickJS-ng on this board: spike measurements, the heap-poisoning/usable_size trap, the job pump, the Xtensa type patches, the DTR/RTS bootloader trap |

Start with the [`lang-c/` README](../lang-c/README.md) for setup + deployment of the working demo; come here for the deeper "how does this actually work" material.
