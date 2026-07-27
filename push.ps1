# push.ps1 - send a script to the board over serial, without reflashing.
#
# The UI is data, not firmware: flash.ps1 rebuilds the firmware, this pushes the
# JavaScript it runs. Uses the host's app-begin/app-end protocol, so the board
# writes the file to storage and reloads that app immediately.
#
#   .\push.ps1 app\apps\weather.js      # -> /apps/weather.js on the board
#   .\push.ps1 app\app.js               # -> /app.js (the launcher)
#   .\push.ps1 app\selftest.js -Dest /app.js   # run the selftest in place of the launcher
#   .\push.ps1 app\apps\clock.js -Port COM7
#
# The destination mirrors the repo layout under app\, because that layout is
# the card layout. Anything outside app\ needs an explicit -Dest.
#
# Two things this exists to get right, both of which produce a file that still
# parses and therefore fails silently on the panel:
#
#   Encoding. .NET's SerialPort defaults to US-ASCII, which rewrites every
#   non-ASCII character as "?" - the degree sign in weather.js being the one
#   that shows up on screen. The port is forced to UTF-8 below.
#
#   Pacing. The host reads serial one character at a time from loop(), so a
#   file pushed as fast as the port accepts it loses bursts in the middle.
#   Lines go out with a gap between them, and the result is read back and
#   checksummed rather than trusted.

param(
    [Parameter(Mandatory = $true)][string]$Path,
    [string]$Dest,
    [string]$Port,
    [int]$LineDelayMs = 25,
    [switch]$NoVerify
)

$ErrorActionPreference = 'Stop'

$Local = Resolve-Path $Path
$AppRoot = Join-Path $PSScriptRoot 'app'

if (-not $Dest) {
    # Mirror the path under app\ onto the card: app\apps\x.js -> /apps/x.js
    $full = $Local.Path
    $root = (Resolve-Path $AppRoot).Path
    if (-not $full.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) {
        throw "$Path is outside $AppRoot, so there is no implied destination. Pass -Dest /apps/whatever.js."
    }
    $Dest = '/' + $full.Substring($root.Length).TrimStart('\', '/').Replace('\', '/')
}

function Find-Port {
    $cmd = Get-Command arduino-cli -ErrorAction SilentlyContinue
    if ($cmd) {
        $json = & $cmd.Source board list --format json 2>$null | ConvertFrom-Json
        foreach ($p in $json.detected_ports) {
            if ($p.matching_boards) { return $p.port.address }
        }
    }
    $ports = [System.IO.Ports.SerialPort]::GetPortNames()
    if ($ports.Count -eq 1) { return $ports[0] }
    if ($ports.Count -eq 0) { throw 'No serial port found. Is it a DATA USB-C cable, and is the board running (not in bootloader)?' }
    throw "Multiple ports found ($($ports -join ', ')). Pick one with -Port COMx."
}

# Read as text and normalise line endings: the host strips \r and re-appends \n
# per line, so CRLF on disk would otherwise make the readback length disagree
# with the file for reasons that have nothing to do with a bad transfer.
$text = [System.IO.File]::ReadAllText($Local) -replace "`r", ''
$lines = $text -split "`n"

# What the board will actually hold. It writes "line + \n" for every line, so
# whatever the file does or does not end with, the stored copy ends with
# exactly one more newline than the text on disk. Comparing against the file
# itself reports a mismatch on every healthy push.
$expected = $text + "`n"

# Position-sensitive checksum over the same text the board should end up
# holding. Kept inside 2^31 so both sides can compute it without overflow.
function Get-ScriptSum([string]$s) {
    $h = [int64]0
    for ($i = 0; $i -lt $s.Length; $i++) {
        $h = ($h + ([int64]($i + 1) * [int64][char]$s[$i])) % 2147483647
    }
    return $h
}

if (-not $Port) { $Port = Find-Port }

Write-Host "file : $Local"
Write-Host "dest : $Dest"
Write-Host "port : $Port"
Write-Host "size : $($text.Length) chars, $($lines.Count) lines`n"

$sp = New-Object System.IO.Ports.SerialPort $Port, 115200, 'None', 8, 'One'
$sp.NewLine = "`n"
# The whole point (see the header): without this, every non-ASCII character
# silently becomes "?".
$sp.Encoding = New-Object System.Text.UTF8Encoding $false
# DTR on, RTS off: asserting RTS is what drops this board into the bootloader.
$sp.DtrEnable = $true
$sp.RtsEnable = $false
$sp.ReadTimeout = 1500
$sp.Open()

try {
    Start-Sleep -Milliseconds 2500   # the native USB CDC re-enumerates on open
    $sp.DiscardInBuffer()

    $sp.WriteLine("app-begin $Dest")
    Start-Sleep -Milliseconds 400
    $n = 0
    foreach ($l in $lines) {
        $sp.WriteLine($l)
        $n++
        if ($n % 20 -eq 0) { Write-Host -NoNewline "`r  sent $n/$($lines.Count) lines" }
        Start-Sleep -Milliseconds $LineDelayMs
    }
    Write-Host "`r  sent $($lines.Count)/$($lines.Count) lines"
    Start-Sleep -Milliseconds 300
    $sp.WriteLine('app-end')
    Start-Sleep -Milliseconds 3000

    $out = New-Object System.Text.StringBuilder
    try { while ($true) { [void]$out.AppendLine($sp.ReadLine()) } } catch { }
    Write-Host ''
    Write-Host $out.ToString().TrimEnd()

    if ($NoVerify) { Write-Host "`nNoVerify set - not reading it back."; return }

    # Read it back. A dropped burst leaves a file that still parses, so
    # "it reloaded without an error" is not evidence that it arrived intact.
    $sp.DiscardInBuffer()
    $js = '(() => { const s = fs.read("' + $Dest + '"); let h = 0; ' +
          'for (let i = 0; i < s.length; i++) h = (h + (i + 1) * s.charCodeAt(i)) % 2147483647; ' +
          'return "SUM " + s.length + " " + h; })()'
    $sp.WriteLine($js)
    Start-Sleep -Milliseconds 2000

    $reply = New-Object System.Text.StringBuilder
    try { while ($true) { [void]$reply.AppendLine($sp.ReadLine()) } } catch { }
    $line = ($reply.ToString() -split "`n" | Where-Object { $_ -match 'SUM (\d+) (\d+)' } | Select-Object -First 1)

    if (-not $line) {
        throw "Could not read the file back from the board. Raw reply:`n$($reply.ToString())"
    }
    $null = $line -match 'SUM (\d+) (\d+)'
    $gotLen = [int]$Matches[1]
    $gotSum = [int64]$Matches[2]
    $wantSum = Get-ScriptSum $expected

    if ($gotLen -eq $expected.Length -and $gotSum -eq $wantSum) {
        Write-Host "`nverified: $gotLen chars, checksum $gotSum" -ForegroundColor Green
    } else {
        Write-Host "`nMISMATCH - the board's copy is not the file you pushed." -ForegroundColor Red
        Write-Host "  length   local $($expected.Length)  board $gotLen"
        Write-Host "  checksum local $wantSum  board $gotSum"
        Write-Host '  Re-run, and if it repeats raise -LineDelayMs (the host reads serial from loop()).'
        exit 1
    }
} finally {
    $sp.Close()
}
