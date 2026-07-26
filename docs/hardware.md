# Hardware: Waveshare ESP32-S3-Touch-LCD-1.47

What this board actually is, and every trap discovered while bringing it up. All of this was verified on real hardware on 2026-07-25 unless marked otherwise.

## The essentials

| Thing | Value |
|---|---|
| MCU | ESP32-S3R8 — dual-core LX7 @ 240 MHz, WiFi b/g/n, BLE 5 |
| Flash / PSRAM | 16 MB flash, 8 MB **octal (OPI)** PSRAM |
| Panel | 1.47" IPS, 172 × 320 native portrait, **JD9853** controller, 4-wire SPI |
| Touch | **AXS5106L** capacitive, I2C addr `0x63`, chip ID reads `51 06 01` |
| USB | ESP32-S3 native USB-Serial/JTAG (`VID_303A PID_1001`) — no CH340/CP2102 |
| Extras | microSD slot (4-bit SDMMC pins), battery ADC, BOOT + RESET buttons |

## Pinout

Cross-checked across three sources (Waveshare wiki, their ESP-IDF BSP headers, and a working community sketch); per-pin citations live in [`board_pins.h`](../lang-c/app/board_pins.h).

| Function | GPIO |
|---|---|
| LCD SCK / MOSI / DC / CS / RST / Backlight | 38 / 39 / 45 / 21 / 40 / 46 |
| Touch I2C SDA / SCL | 42 / 41 |
| Touch RST + INT (ambiguous — see [touch.md](lang-c/touch.md)) | 47, 48 |
| microSD CLK / CMD / D0–D3 | 16 / 15 / 17, 18, 13, 14 |
| Battery ADC (÷2 divider) | 12 |
| BOOT button | 0 |

## Traps, in order of how much time they can waste

1. **PSRAM must be set to OPI, not QSPI.** The S3**R8** suffix means octal PSRAM. The wrong Arduino setting produces a boot loop that looks like broken firmware.
2. **The board-menu entry `Waveshare ESP32-S3-LCD-1.47` is the WRONG board.** That's the *non-touch* variant with a different pinout — its GPIO38 drives a WS2812 LED; on this board GPIO38 is the LCD clock. Use **ESP32S3 Dev Module** and let the sketch define its own pins.
3. **This board has NO addressable RGB LED.** Don't port WS2812 blink examples to it expecting one.
4. **GPIO12 (battery) is an ADC2 channel.** ADC2 is arbitrated by the WiFi driver — reads return 0 while the radio is active. The sketch surfaces this as "bat n/a" instead of a fake 0.00 V. The ÷2 divider is inferred from community code, not a schematic; treat readings as indicative.
5. **Charge-only USB-C cables enumerate nothing.** This cost a whole session: the board was "plugged in" but Windows saw no `VID_303A` device at all. With a data cable it shows up instantly as a `USB Serial Device (COMx)` plus a `USB JTAG/serial debug unit`. If in doubt: `Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -match 'VID_303A' }`

## What's the microSD slot for?

**Nothing, by default — it's optional storage.** No demo requirement, and this project never touches it (pins 13–18 sit unused; a card can be inserted or not, it makes no difference). Sketches that *want* it typically use it for:

- logging sensor data over long periods
- image / font / audio assets too big for flash
- configuration files you can edit from a PC

It's wired for 4-bit SDMMC (CLK 16, CMD 15, D0–D3 = 17/18/13/14), which is the fast mode — use the ESP32 `SD_MMC` library, not the SPI `SD` library, if you add support. A natural extension of this demo would be an "SD" tab that mounts the card and shows capacity/contents.

## Power / battery

There's a battery connector with GPIO12 sensing the rail through a divider. USB powers everything during development; nothing in this project needs a battery.
