# flash.ps1 - build + upload app from the command line.
#
# Handy alternative to clicking through Arduino IDE, and it prints the exact
# board options so nothing depends on remembering menu state.
#
#   .\flash.ps1              # auto-detect the port, build, upload, then monitor
#   .\flash.ps1 -Port COM7   # force a port
#   .\flash.ps1 -BuildOnly   # compile only, don't touch the board
#   .\flash.ps1 -Monitor     # just open the serial monitor

param(
    [string]$Port,
    [switch]$BuildOnly,
    [switch]$Monitor
)

$ErrorActionPreference = 'Stop'

$Sketch = Join-Path $PSScriptRoot 'app'
$Fqbn   = 'esp32:esp32:esp32s3:' + (@(
    'PSRAM=opi'                          # ESP32-S3R8 = OCTAL psram. Wrong value -> boot loop.
    'FlashSize=16M'
    'PartitionScheme=app3M_fat9M_16MB'   # default 4MB scheme is too small for this sketch
    'CDCOnBoot=cdc'                      # needed for Serial over the native USB-C port
    'FlashMode=qio'
    'USBMode=hwcdc'
) -join ',')

function Get-Cli {
    $cmd = Get-Command arduino-cli -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    # Fall back to the copy bundled inside Arduino IDE.
    $bundled = 'C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe'
    if (Test-Path $bundled) { return $bundled }
    throw 'arduino-cli not found. Install it, or use Arduino IDE directly (see README.md).'
}

function Find-Port {
    # Prefer a port arduino-cli itself recognises as an ESP32 board.
    $json = & $cli board list --format json 2>$null | ConvertFrom-Json
    foreach ($p in $json.detected_ports) {
        if ($p.matching_boards) { return $p.port.address }
    }
    # Otherwise: if exactly one serial port exists, assume it's the board.
    $ports = [System.IO.Ports.SerialPort]::GetPortNames()
    if ($ports.Count -eq 1) { return $ports[0] }
    if ($ports.Count -eq 0) {
        throw @"
No serial port found. Check, in order:
  1. Is it a DATA USB-C cable? Charge-only cables enumerate nothing.
  2. Try the bootloader: hold BOOT, tap RESET, release BOOT, then re-run.
  3. Confirm Windows sees it - an Espressif board shows up as VID_303A:
       Get-PnpDevice -PresentOnly | Where-Object { `$_.InstanceId -match 'VID_303A' }
"@
    }
    throw "Multiple ports found ($($ports -join ', ')). Pick one with -Port COMx."
}

$cli = Get-Cli
Write-Host "arduino-cli : $cli"
Write-Host "sketch      : $Sketch"
Write-Host "fqbn        : $Fqbn`n"

if ($Monitor) {
    if (-not $Port) { $Port = Find-Port }
    Write-Host "Monitoring $Port at 115200 (Ctrl+C to stop)`n"
    & $cli monitor -p $Port --config baudrate=115200
    return
}

Write-Host '--- compiling ---'
& $cli compile -b $Fqbn --warnings all $Sketch
if ($LASTEXITCODE -ne 0) { throw 'Compile failed.' }
Write-Host "compile OK`n"

if ($BuildOnly) { Write-Host 'BuildOnly set - not uploading.'; return }

if (-not $Port) { $Port = Find-Port }
Write-Host "--- uploading to $Port ---"
& $cli upload -b $Fqbn -p $Port $Sketch
if ($LASTEXITCODE -ne 0) {
    throw @"
Upload failed. Most common cause is the board not being in download mode:
  hold BOOT, tap RESET, release BOOT, then re-run this script.
Note the port number often CHANGES in bootloader mode - re-check with -Port.
"@
}

Write-Host "`nupload OK - opening monitor (Ctrl+C to stop)`n"
& $cli monitor -p $Port --config baudrate=115200
