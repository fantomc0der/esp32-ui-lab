# Building & deploying

Everything here targets the **Waveshare ESP32-S3-Touch-LCD-1.47** over its native USB-C port using [arduino-cli](https://arduino.github.io/arduino-cli/). The Arduino IDE works too (each sketch folder opens directly), but the scripts and commands below assume the CLI.

## One-time setup

```powershell
arduino-cli core update-index
arduino-cli core install esp32:esp32@3.3.11
arduino-cli lib install "lvgl@9.5.0"
arduino-cli lib install "GFX Library for Arduino@1.6.7"
```

These are the versions the repo is verified against; newer ones usually work but haven't been proven on this hardware. No LVGL configuration is needed in the libraries folder — each sketch carries its own `lv_conf.h`, which LVGL picks up automatically.

## The FQBN (get this right or the board boot-loops)

Every compile/upload uses this exact board spec:

```
esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=cdc,FlashMode=qio,USBMode=hwcdc
```

The two you must never change: `PSRAM=opi` (the S3**R8** has octal PSRAM; `qspi` or disabled = boot loop) and `CDCOnBoot=cdc` (without it serial over the USB-C port is silent). `app3M_fat9M_16MB` gives the 3 MB app partition both demos need plus the 9.9 MB FATFS partition the JS runtime can load `app.js` from. Full rationale: [`docs/lang-c/build-and-flash.md`](docs/lang-c/build-and-flash.md).

## The easy path: flash scripts

Both language directories ship a wrapper that encodes the FQBN, finds the port, compiles, uploads, and opens a monitor:

```powershell
cd lang-c
.\flash.ps1                 # build + flash the C demo (WaveshareVitals)

cd lang-js
.\flash.ps1                 # build + flash the JS runtime (JsHost)
```

Common flags for either script: `-BuildOnly` (compile without touching the board), `-Port COM7` (skip auto-detection), `-Monitor` (just open the serial monitor at 115200).

## Manual CLI equivalents

```powershell
$fqbn = 'esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=cdc,FlashMode=qio,USBMode=hwcdc'

# C demo
arduino-cli compile -b $fqbn .\lang-c\WaveshareVitals
arduino-cli upload  -b $fqbn -p COM4 .\lang-c\WaveshareVitals

# JS runtime — note the extra --library flag linking the vendored engine
arduino-cli compile --library .\lang-js\quickjs-ng -b $fqbn .\lang-js\JsHost
arduino-cli upload  -b $fqbn -p COM4 .\lang-js\JsHost

# serial monitor (either sketch)
arduino-cli monitor -p COM4 --config baudrate=115200
```

## Deploying JavaScript (JS way only)

Flashing JsHost is a one-time step; after that the UI is data, not firmware. Get `app.js` onto the board either way:

- **SD card:** copy [`lang-js/app/app.js`](lang-js/app/app.js) to the root of a FAT-formatted microSD, insert, long-press **BOOT** (≥ 700 ms). The card wins over the flash partition and is re-mounted on every reload, so it can be swapped while powered.
- **Over serial, no card handling:** send the line `app-begin`, then the script's lines, then `app-end` — JsHost writes it to the internal FATFS partition and reloads immediately. Typing `reload` re-reads storage; any other line is evaluated as JavaScript in the running app (live REPL).

## If something misbehaves

The three that account for nearly all bring-up pain: a **charge-only USB cable** enumerates nothing at all (no error, just silence); **only one process can hold the COM port**, so close any monitor before uploading; and if an upload can't connect, hold BOOT, tap RESET, release BOOT, and retry (the COM number often changes in bootloader mode). The full Windows serial survival guide — phantom ports, scripted capture, the DTR/RTS trap that can park the chip in the ROM bootloader — is in [`docs/lang-c/build-and-flash.md`](docs/lang-c/build-and-flash.md) and [`docs/lang-js/engine-notes.md`](docs/lang-js/engine-notes.md).
