# Build & flash reference

Everything needed to rebuild and deploy this project, including the CLI paths and the Windows serial tricks used during bring-up.

## Toolchain (verified versions)

| Component | Version |
|---|---|
| esp32 Arduino core (Espressif) | 3.3.11 |
| lvgl | 9.5.0 |
| GFX Library for Arduino (Moon On Our Nation) | 1.6.7 |
| arduino-cli | `C:\Users\micha\.local\bin\arduino-cli.exe` (also bundled inside Arduino IDE) |

## The FQBN — the settings that matter

```
esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=cdc,FlashMode=qio,USBMode=hwcdc
```

| Option | Why |
|---|---|
| `PSRAM=opi` | ESP32-S3**R8** = octal PSRAM. `qspi`/disabled → boot loop. |
| `PartitionScheme=app3M_fat9M_16MB` | Default 4 MB scheme gives 1.2 MB app; this sketch is ~1.26 MB. (Note: NOT `default_16MB` — that ID doesn't exist.) |
| `CDCOnBoot=cdc` | Without it, `Serial` over the native USB-C port is silent. |
| `USBMode=hwcdc` | Native USB-Serial/JTAG mode (what the board actually has). |

`flash.ps1` in the `lang-c/` folder encodes all of this: `.\flash.ps1` / `-BuildOnly` / `-Port COMx` / `-Monitor`.

## Manual CLI equivalents

```powershell
arduino-cli compile -b <FQBN> --warnings all .\WaveshareVitals
arduino-cli upload  -b <FQBN> -p COM4 .\WaveshareVitals
arduino-cli monitor -p COM4 --config baudrate=115200
```

## USB / port facts (learned the hard way)

- The board is the ESP32-S3's **native** USB: enumerates as `USB Serial Device (COMx)` + `USB JTAG/serial debug unit`, `VID_303A PID_1001`. **No driver needed on Windows 11.**
- **A charge-only cable enumerates nothing at all** — no error, no unknown device, just silence. First thing to check, always.
- Check enumeration:
  ```powershell
  [System.IO.Ports.SerialPort]::GetPortNames()
  Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -match 'VID_303A' }
  ```
  A port that shows in Device Manager but not in `GetPortNames()` is a *phantom* (remembered, not present).
- If upload fails to connect: hold **BOOT**, tap **RESET**, release BOOT, retry. The COM number often changes in bootloader mode.
- **Only one process can hold the port.** An upload fails with `exit status 2` if a monitor/logger has COM4 open — close it first.

## Scripted serial capture (how touch calibration was recorded)

Reading the port programmatically instead of eyeballing a monitor window — useful for any timed capture:

```powershell
$p = New-Object System.IO.Ports.SerialPort 'COM4',115200,'None',8,'One'
$p.DtrEnable = $true; $p.RtsEnable = $true   # without DTR, native-USB CDC may not transmit
$p.Open()
$deadline = (Get-Date).AddSeconds(90); $buf = ''
while ((Get-Date) -lt $deadline) { try { $buf += $p.ReadExisting() } catch {}; Start-Sleep -Milliseconds 100 }
$p.Close(); $buf
```

## Expected boot log

```
[boot] WaveshareVitals starting
[boot] display 320x172
[touch] AXS5106L ok, id = 51 06 01
[boot] lvgl draw buffers: 13440 bytes each
[boot] ready
```

Anything else, see the troubleshooting table in the C-way [README](../../lang-c/README.md). Reopen the serial monitor after each reset — native USB re-enumerates and the old handle dies.

## Build stats (final verified build)

- 1 261 126 bytes flash (40% of 3 MB app partition)
- 96 788 bytes static RAM (29%)
- compiles with `--warnings all`; the only warnings are inside the GFX library (`Print::flush` hiding) and the linker's executable-stack note — neither is ours, both are harmless.
