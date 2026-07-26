# flash.ps1 - build + upload a lang-js sketch from the command line.
#
# Same idea as lang-c/flash.ps1, with three differences: the target board is
# selectable (-Target), the sketch is selectable (-Sketch, js-host by default),
# and every build links the shared libraries beside it (quickjs-ng,
# lvgl-js-bindings) via --library.
#
#   .\flash.ps1                    # build js-host for the Waveshare board, upload, monitor
#   .\flash.ps1 -Target cyd        # build for the ESP32-2432S028R instead
#   .\flash.ps1 -Port COM7         # force a port
#   .\flash.ps1 -BuildOnly         # compile only
#   .\flash.ps1 -Monitor           # just open the serial monitor
#
# Each target owns its FQBN and the -D that picks its pin map, because the two
# boards are different CHIPS (S3 vs classic ESP32), not just different pinouts.

param(
    [ValidateSet('waveshare', 'cyd')]
    [string]$Target = 'waveshare',
    [string]$Sketch = 'js-host',
    [string]$Port,
    [switch]$BuildOnly,
    [switch]$Monitor
)

$ErrorActionPreference = 'Stop'

$SketchPath = Join-Path $PSScriptRoot $Sketch
# Vendored/shared code lives in libraries beside the sketches, not inside them.
$Libs = @(
    Join-Path $PSScriptRoot 'quickjs-ng'        # the JS engine
    Join-Path $PSScriptRoot 'lvgl-js-bindings'  # the LVGL bindings
) | ForEach-Object { '--library', $_ }

$Targets = @{
    # Waveshare ESP32-S3-Touch-LCD-1.47: JD9853 172x320, capacitive touch, 8MB
    # octal PSRAM, 16MB flash with a FATFS partition for scripts.
    waveshare = @{
        Name = 'Waveshare ESP32-S3-Touch-LCD-1.47'
        Fqbn = 'esp32:esp32:esp32s3:' + (@(
            'PSRAM=opi'                          # ESP32-S3R8 = OCTAL psram. Wrong value -> boot loop.
            'FlashSize=16M'
            'PartitionScheme=app3M_fat9M_16MB'   # 3MB app + the 9.9MB FATFS that can hold app.js
            'CDCOnBoot=cdc'                      # needed for Serial over the native USB-C port
            'FlashMode=qio'
            'USBMode=hwcdc'
        ) -join ',')
        Define = 'BOARD_WAVESHARE_S3_147'
        # JS heap in PSRAM.
        HeapCaps = 'MALLOC_CAP_SPIRAM'
        # Stock LVGL widget pool: PSRAM means nothing competes for internal RAM.
        LvMemKB = 48
        # 20 KB of JS recursion inside a 32 KB loop-task stack.
        LoopStackKB = 32
        JsStackKB   = 20
        # Full JS language surface: PSRAM means there is nothing to save by trimming.
        LeanContext = 0
    }
    # ESP32-2432S028R "Cheap Yellow Display": classic ESP32, ILI9341 240x320,
    # resistive XPT2046 touch, NO PSRAM, 4MB flash (so scripts come from SD).
    cyd = @{
        Name = 'ESP32-2432S028R (CYD)'
        Fqbn = 'esp32:esp32:esp32:' + (@(
            'PSRAM=disabled'                     # this board has none at all
            'FlashSize=4M'
            'PartitionScheme=huge_app'           # 3MB app; 4MB leaves no room for a script partition
            'FlashMode=qio'
            'CPUFreq=240'
        ) -join ',')
        Define = 'BOARD_CYD_2432S028R'
        # No PSRAM, so the JS heap comes from internal RAM — and the DMA cap is
        # mandatory, not cosmetic. Bare MALLOC_CAP_INTERNAL can return IRAM,
        # which only allows aligned 32-bit access, and QuickJS writes bytes and
        # shorts across its heap: that panics with LoadStoreError inside
        # JS_NewRuntime2. MALLOC_CAP_DMA forces byte-addressable DRAM.
        HeapCaps = '(MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA)'
        # Halved from the stock 48: LVGL's pool is static .bss competing with the
        # JS heap for the same internal SRAM. See board_config.h for the budget.
        LvMemKB = 24
        # Same 32 KB task stack as the other target, but a smaller JS budget inside
        # it. QuickJS derives its overflow threshold as (current SP - JsStackKB), so
        # the budget must sit comfortably inside what the task has LEFT at the point
        # the VM starts: 20 KB in 32 KB put the threshold below the real stack base
        # here and made every eval fail on entry.
        #
        # Shrink the budget rather than growing the stack: the loop-task stack is
        # allocated from the same internal DRAM as the JS heap, so a 48 KB stack
        # cost 16 KB of heap and halved the largest free block (69620 -> 31732),
        # which broke context creation instead.
        LoopStackKB = 32
        JsStackKB   = 12
        # Trim the context to the intrinsics the shipped scripts actually use.
        # JS_NewContext() loads twelve groups regardless, and startup is what
        # exhausts the heap here (~80 KB of the ~81 KB available).
        LeanContext = 1
    }
}

$Cfg  = $Targets[$Target]
$Fqbn = $Cfg.Fqbn

# These have to be -D rather than #define in a header because each one is read by
# code that cannot see the sketch's board_config.h: JS_HEAP_CAPS by the bindings
# library, BOARD_LV_MEM_KB by LVGL's own compilation units via lv_conf.h.
$BuildFlags = "-D$($Cfg.Define) -DJS_HEAP_CAPS=$($Cfg.HeapCaps) -DBOARD_LV_MEM_KB=$($Cfg.LvMemKB)" +
              " -DBOARD_LOOP_STACK_KB=$($Cfg.LoopStackKB) -DJS_MAX_STACK=$($Cfg.JsStackKB * 1024)" +
              " -DJS_LEAN_CONTEXT=$($Cfg.LeanContext)"
$BuildProps = @('--build-property', "compiler.cpp.extra_flags=$BuildFlags",
                '--build-property', "compiler.c.extra_flags=$BuildFlags")

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
Write-Host "target      : $Target - $($Cfg.Name)"
Write-Host "sketch      : $SketchPath"
Write-Host "fqbn        : $Fqbn"
Write-Host "flags       : $BuildFlags`n"

if ($Monitor) {
    if (-not $Port) { $Port = Find-Port }
    Write-Host "Monitoring $Port at 115200 (Ctrl+C to stop)`n"
    & $cli monitor -p $Port --config baudrate=115200
    return
}

# Build into a per-target directory. Two reasons: the upload step needs to be
# pointed at the artefacts explicitly (arduino-cli's `upload` does not accept
# --build-property, so it cannot re-derive the flag-hashed default path), and
# keeping targets in separate directories stops an S3 binary and an ESP32 binary
# from overwriting each other between builds.
$BuildDir = Join-Path $PSScriptRoot "build\$Target"

Write-Host '--- compiling ---'
& $cli compile @Libs @BuildProps --build-path $BuildDir -b $Fqbn $SketchPath
if ($LASTEXITCODE -ne 0) { throw 'Compile failed.' }
Write-Host "compile OK`n"

if ($BuildOnly) { Write-Host 'BuildOnly set - not uploading.'; return }

if (-not $Port) { $Port = Find-Port }
Write-Host "--- uploading to $Port ---"
# --input-dir, not the sketch path: `upload` rejects --build-property, so it
# cannot work out where a flag-customised build landed. Point it at the output.
& $cli upload --input-dir $BuildDir -b $Fqbn -p $Port
if ($LASTEXITCODE -ne 0) {
    throw @"
Upload failed. Most common cause is the board not being in download mode:
  hold BOOT, tap RESET, release BOOT, then re-run this script.
Note the port number often CHANGES in bootloader mode - re-check with -Port.
"@
}

Write-Host "`nupload OK - opening monitor (Ctrl+C to stop)`n"
& $cli monitor -p $Port --config baudrate=115200
