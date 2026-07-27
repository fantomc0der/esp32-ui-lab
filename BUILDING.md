# Building & deploying

Everything here targets the **Waveshare ESP32-S3-Touch-LCD-1.47** over its native USB-C port using [arduino-cli](https://arduino.github.io/arduino-cli/) (the Arduino IDE works too — each sketch folder opens directly). This page covers what's common to everything in the repo, with the detailed docs linked below.

## One-time setup

```powershell
arduino-cli core update-index
arduino-cli core install esp32:esp32@3.3.11
arduino-cli lib install "lvgl@9.5.0"
arduino-cli lib install "GFX Library for Arduino@1.6.7"
```

These are the versions the repo is verified against; newer ones usually work but haven't been proven on this hardware. No LVGL configuration is needed in the libraries folder — each sketch carries its own `lv_conf.h`, which LVGL picks up automatically.

## The FQBN (get this right or the board boot-loops)

Every compile/upload in this repo uses this exact board spec:

```
esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=cdc,FlashMode=qio,USBMode=hwcdc
```

The two you must never change: `PSRAM=opi` (the S3**R8** has octal PSRAM; anything else = boot loop) and `CDCOnBoot=cdc` (without it serial over the USB-C port is silent). Full rationale for every option: [`docs/experiments/c-dashboard/build-and-flash.md`](docs/experiments/c-dashboard/build-and-flash.md).

## Building and flashing

`flash.ps1` encodes the FQBN, finds the port, compiles, uploads, and opens a monitor. Flags: `-BuildOnly`, `-Port COMx`, `-Monitor`, and `-Board` to pick one of `firmware/boards/*`.

### The firmware — [`firmware/`](firmware/README.md)

```powershell
.\flash.ps1
```

Flashing is a one-time step; after that the UI is data. You deploy `app.js` by SD card + BOOT long-press or over serial, with no recompile. The build links the two libraries in `firmware/` with `--library`, because they sit beside the board sketches rather than in the Arduino libraries folder. Details (manual CLI, deploying app.js, the serial REPL and upload protocol, expected boot log): [`docs/build-and-deploy.md`](docs/build-and-deploy.md).

### The frozen C dashboard — [`experiments/c-dashboard/`](experiments/c-dashboard/README.md)

```powershell
cd experiments/c-dashboard; .\flash.ps1
```

One firmware image containing the whole dashboard, with no scripting; changing the UI means recompiling and reflashing. Details (manual CLI commands, expected boot log, scripted serial capture): [`docs/experiments/c-dashboard/build-and-flash.md`](docs/experiments/c-dashboard/build-and-flash.md).

## CI

[`.github/workflows/ci.yml`](.github/workflows/ci.yml) compiles every board in `firmware/boards/` plus the frozen C dashboard on each push and pull request, using the same pinned versions listed above, and checks the JavaScript: every script is syntax-checked, and [`tools/check-js-api.mjs`](tools/check-js-api.mjs) fails the build if a script calls a binding the C layer doesn't actually register — the mistake that would otherwise only surface on the device.

What CI cannot do is run the real test suite. [`app/selftest.js`](app/selftest.js) executes on the board and reports over serial, so it needs hardware; a hosted runner has none. Run it yourself after any change to the binding layer (see [`docs/build-and-deploy.md`](docs/build-and-deploy.md)), or attach a board to a self-hosted runner if you want it automated.

## If something misbehaves

The three that account for nearly all bring-up pain: a **charge-only USB cable** enumerates nothing at all (no error, just silence); **only one process can hold the COM port**, so close any monitor before uploading; and if an upload can't connect, hold BOOT, tap RESET, release BOOT, and retry (the COM number often changes in bootloader mode). The full Windows serial survival guide — phantom ports, scripted capture, the DTR/RTS trap that can park the chip in the ROM bootloader — is in [`docs/experiments/c-dashboard/build-and-flash.md`](docs/experiments/c-dashboard/build-and-flash.md) and [`docs/engine-notes.md`](docs/engine-notes.md).
