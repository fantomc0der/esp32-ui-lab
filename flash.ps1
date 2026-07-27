# flash.ps1 - build + upload a board firmware from the command line.
#
# Builds one of firmware/boards/*, linking the two shared libraries
# (quickjs-ng, lvgl-js-bindings) via --library because they live in
# firmware/ rather than in the Arduino libraries folder.
#
#   .\flash.ps1                            # build the default board, upload, monitor
#   .\flash.ps1 -Board other-board         # build a different board
#   .\flash.ps1 -Port COM7                 # force a port
#   .\flash.ps1 -BuildOnly                 # compile only
#   .\flash.ps1 -Monitor                   # just open the serial monitor

param(
    [string]$Board = 'waveshare-s3-touch-147',
    [string]$Port,
    [switch]$BuildOnly,
    [switch]$Monitor
)

$ErrorActionPreference = 'Stop'

$Firmware   = Join-Path $PSScriptRoot 'firmware'
$SketchPath = Join-Path $Firmware "boards/$Board"
if (-not (Test-Path $SketchPath)) {
    $available = (Get-ChildItem (Join-Path $Firmware 'boards') -Directory).Name -join ', '
    throw "No such board '$Board'. Available: $available"
}
# Vendored/shared code lives in libraries beside the boards, not inside them.
$Libs = @(
    Join-Path $Firmware 'quickjs-ng'        # the JS engine
    Join-Path $Firmware 'lvgl-js-bindings'  # the LVGL bindings
) | ForEach-Object { '--library', $_ }
$Fqbn   = 'esp32:esp32:esp32s3:' + (@(
    'PSRAM=opi'                          # ESP32-S3R8 = OCTAL psram. Wrong value -> boot loop.
    'FlashSize=16M'
    'PartitionScheme=app3M_fat9M_16MB'   # 3MB app + the 9.9MB FATFS that can hold app.js
    'CDCOnBoot=cdc'                      # needed for Serial over the native USB-C port
    'FlashMode=qio'
    'USBMode=hwcdc'
) -join ',')

function Get-Cli {
    $cmd = Get-Command arduino-cli -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $bundled = 'C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe'
    if (Test-Path $bundled) { return $bundled }
    throw 'arduino-cli not found. Install it, or use Arduino IDE directly.'
}

function Find-Port {
    $json = & $cli board list --format json 2>$null | ConvertFrom-Json
    foreach ($p in $json.detected_ports) {
        if ($p.matching_boards) { return $p.port.address }
    }
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
Write-Host "board       : $Board"
Write-Host "sketch      : $SketchPath"
Write-Host "fqbn        : $Fqbn`n"

if ($Monitor) {
    if (-not $Port) { $Port = Find-Port }
    Write-Host "Monitoring $Port at 115200 (Ctrl+C to stop)`n"
    & $cli monitor -p $Port --config baudrate=115200
    return
}

Write-Host '--- compiling ---'
& $cli compile @Libs -b $Fqbn $SketchPath
if ($LASTEXITCODE -ne 0) { throw 'Compile failed.' }
Write-Host "compile OK`n"

if ($BuildOnly) { Write-Host 'BuildOnly set - not uploading.'; return }

if (-not $Port) { $Port = Find-Port }
Write-Host "--- uploading to $Port ---"
& $cli upload -b $Fqbn -p $Port $SketchPath
if ($LASTEXITCODE -ne 0) {
    throw @"
Upload failed. Most common cause is the board not being in download mode:
  hold BOOT, tap RESET, release BOOT, then re-run this script.
Note the port number often CHANGES in bootloader mode - re-check with -Port.
"@
}

Write-Host "`nupload OK - opening monitor (Ctrl+C to stop)`n"
& $cli monitor -p $Port --config baudrate=115200
