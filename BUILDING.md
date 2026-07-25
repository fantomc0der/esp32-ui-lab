# Building & deploying

Everything here targets the **Waveshare ESP32-S3-Touch-LCD-1.47** over its native USB-C port using [arduino-cli](https://arduino.github.io/arduino-cli/) (the Arduino IDE works too — each sketch folder opens directly). This page covers what's common to every project in the repo; each `lang-*/` way has its own detailed doc, linked below.

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

The two you must never change: `PSRAM=opi` (the S3**R8** has octal PSRAM; anything else = boot loop) and `CDCOnBoot=cdc` (without it serial over the USB-C port is silent). Full rationale for every option: [`docs/lang-c/build-and-flash.md`](docs/lang-c/build-and-flash.md).

## Building and flashing

Each way ships a `flash.ps1` that encodes the FQBN, finds the port, compiles, uploads, and opens a monitor. Shared flags: `-BuildOnly`, `-Port COMx`, `-Monitor`.

### The C way — [`lang-c/`](lang-c/README.md)

```powershell
cd lang-c; .\flash.ps1
```

One firmware image containing the whole demo; changing the UI means recompiling and reflashing. Details (manual CLI commands, expected boot log, scripted serial capture): [`docs/lang-c/build-and-flash.md`](docs/lang-c/build-and-flash.md).

### The JS way — [`lang-js/`](lang-js/README.md)

```powershell
cd lang-js; .\flash.ps1
```

Flash the JsHost runtime once; after that the UI is an `app.js` file you deploy as data — SD card + BOOT long-press, or pasted over serial — with no recompile. Builds add one flag over the C way (`--library` for the vendored JS engine). Details (manual CLI, deploying app.js, the serial REPL and upload protocol, expected boot log): [`docs/lang-js/build-and-deploy.md`](docs/lang-js/build-and-deploy.md).

## If something misbehaves

The three that account for nearly all bring-up pain: a **charge-only USB cable** enumerates nothing at all (no error, just silence); **only one process can hold the COM port**, so close any monitor before uploading; and if an upload can't connect, hold BOOT, tap RESET, release BOOT, and retry (the COM number often changes in bootloader mode). The full Windows serial survival guide — phantom ports, scripted capture, the DTR/RTS trap that can park the chip in the ROM bootloader — is in [`docs/lang-c/build-and-flash.md`](docs/lang-c/build-and-flash.md) and [`docs/lang-js/engine-notes.md`](docs/lang-js/engine-notes.md).
