# Docs index

Topic notes for the WaveshareVitals project — written so future-you (or a future
session) can recall *why* things are the way they are without re-deriving them.

| Doc | What's in it |
|---|---|
| [hardware.md](hardware.md) | The board itself: pinout, chip quirks, what the microSD slot is for, traps specific to this exact Waveshare variant |
| [display-pipeline.md](display-pipeline.md) | How pixels get from LVGL to the JD9853 panel: driver workaround, byte order, buffers, rotation |
| [touch.md](touch.md) | The AXS5106L driver, the I2C protocol, and the full story of the axis-mapping bug — how it was found and fixed with hardware measurement |
| [portability.md](portability.md) | Exactly which code is specific to this device vs. reusable on other ESP32/LVGL hardware, file by file |
| [build-and-flash.md](build-and-flash.md) | Arduino board settings, the FQBN, flashing from the CLI, and reading the serial port programmatically on Windows |

Start with the top-level [README](../README.md) for setup + deployment; come
here for the deeper "how does this actually work" material.
