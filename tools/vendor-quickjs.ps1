<#
.SYNOPSIS
Re-vendor firmware/quickjs-ng/src/ from upstream QuickJS-ng.

.DESCRIPTION
Clones upstream into a scratch workspace under .temp/, checks out the requested target,
copies the vendored file manifest into firmware/quickjs-ng/src/, and updates the pin
recorded in README.md and library.properties. The copy is unmodified upstream.

Run with the currently pinned SHA to verify reproducibility: `git diff` must come back empty.

.PARAMETER Target
Upstream tag or commit SHA to vendor, e.g. `v0.16.0` or a 40-char SHA.

.PARAMETER Version
Version string to record. Defaults to the tag `Target` resolves to, with any leading `v`
stripped. Required when the target is not tagged.

.PARAMETER KeepWorkspace
Leave the .temp/ workspace in place instead of deleting it.

.EXAMPLE
.\tools\vendor-quickjs.ps1 -Target fd0a0210b7be00957751871e7e01b8291268fc29

.EXAMPLE
.\tools\vendor-quickjs.ps1 -Target v0.16.0
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Target,
    [string]$Version,
    [switch]$KeepWorkspace
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
# Pinned rather than inherited. When this is on, a non-zero exit from a native command raises a
# terminating error, which would bypass every $LASTEXITCODE check below — including the ones whose
# whole job is to turn an *expected* git failure into a useful message or a $null return, such as
# an unresolvable target or a commit that carries no exact tag. It is off by default, but it is a
# preference variable and a profile can flip it, so the script states what it needs.
$PSNativeCommandUseErrorActionPreference = $false

$RepoRoot    = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$VendorDir   = Join-Path $RepoRoot 'firmware/quickjs-ng'
$SrcDir      = Join-Path $VendorDir 'src'
$ReadmePath  = Join-Path $VendorDir 'README.md'
$PropsPath   = Join-Path $VendorDir 'library.properties'
$LicensePath = Join-Path $VendorDir 'LICENSE'
$Workspace   = Join-Path $RepoRoot '.temp/vendor-quickjs'
$ClonePath   = Join-Path $Workspace 'quickjs'
$UpstreamUrl = 'https://github.com/quickjs-ng/quickjs'

# Explicit, not a glob. These are upstream's four CMake `qjs_sources` plus the header closure
# they include. quickjs-libc.c is excluded on purpose: it wants processes, fds and sockets,
# and the sketches supply their own bindings. A glob would silently re-import it the first
# time upstream reorganises the tree; an explicit list fails at the link step instead, which
# is the failure we want.
$Manifest = @(
    'builtin-array-fromasync.h'
    'builtin-iterator-zip-keyed.h'
    'builtin-iterator-zip.h'
    'cutils.h'
    'dtoa.c'
    'dtoa.h'
    'libregexp-opcode.h'
    'libregexp.c'
    'libregexp.h'
    'libunicode-table.h'
    'libunicode.c'
    'libunicode.h'
    'list.h'
    'quickjs-atom.h'
    'quickjs-c-atomics.h'
    'quickjs-opcode.h'
    'quickjs.c'
    'quickjs.h'
)

function Invoke-Git {
    param([Parameter(Mandatory = $true)][string[]]$Arguments, [string]$WorkingDirectory)
    $prev = $null
    if ($WorkingDirectory) { $prev = (Get-Location).Path; Set-Location $WorkingDirectory }
    try {
        & git @Arguments
        if ($LASTEXITCODE -ne 0) {
            throw "git $($Arguments -join ' ') failed with exit code $LASTEXITCODE"
        }
    } finally {
        if ($prev) { Set-Location $prev }
    }
}

# Returns $null when git fails. `git rev-parse` in particular echoes its own argument back on
# stdout when it cannot resolve it, so callers must gate on the exit code rather than on output
# being non-empty.
function Get-GitOutput {
    param([Parameter(Mandatory = $true)][string[]]$Arguments, [string]$WorkingDirectory)
    $prev = $null
    if ($WorkingDirectory) { $prev = (Get-Location).Path; Set-Location $WorkingDirectory }
    try {
        $out = & git @Arguments 2>$null
        if ($LASTEXITCODE -ne 0) { return $null }
        return ($out | Out-String).Trim()
    } finally {
        if ($prev) { Set-Location $prev }
    }
}

# The pin recorded in README.md is the single source of truth for what src/ holds.
$readme = Get-Content -Raw -LiteralPath $ReadmePath
if ($readme -notmatch '\*\*v(?<ver>[0-9][^*]*)\*\* \(commit `(?<sha>[0-9a-f]{40})`\)') {
    throw "Could not find the pin in $ReadmePath. Expected a line matching: **v<version>** (commit ``<40-char sha>``)"
}
$PinnedSha     = $Matches['sha']
$PinnedVersion = $Matches['ver']
Write-Host "Current pin: v$PinnedVersion ($PinnedSha)"

if (Test-Path -LiteralPath $Workspace) { Remove-Item -LiteralPath $Workspace -Recurse -Force }
New-Item -ItemType Directory -Force -Path $Workspace | Out-Null

try {
    Write-Host "Cloning $UpstreamUrl ..."
    Invoke-Git -Arguments @('clone', '--filter=blob:none', '--quiet', $UpstreamUrl, $ClonePath)

    $targetSha = Get-GitOutput -Arguments @('rev-parse', '--verify', '--quiet', "$Target^{commit}") -WorkingDirectory $ClonePath
    if (-not $targetSha) { throw "Target '$Target' does not resolve to a commit in $UpstreamUrl" }

    Write-Host "Checking out $Target ($targetSha) ..."
    Invoke-Git -Arguments @('checkout', '--quiet', $targetSha) -WorkingDirectory $ClonePath

    # Resolve the version to record.
    if (-not $Version) {
        $tag = Get-GitOutput -Arguments @('describe', '--tags', '--exact-match', $targetSha) -WorkingDirectory $ClonePath
        if (-not $tag) { throw "Target '$Target' is not an exact tag; pass -Version explicitly." }
        $Version = $tag -replace '^v', ''
    }
    Write-Host "Recording version $Version at $targetSha"

    # Check every manifest name resolves before anything on disk is touched. This is the failure
    # the explicit manifest is designed to have, so it must land cleanly: discovered mid-copy it
    # would leave src/ half old and half new.
    $missing = @(($Manifest + 'LICENSE') | Where-Object { -not (Test-Path -LiteralPath (Join-Path $ClonePath $_)) })
    if ($missing.Count -gt 0) {
        throw ("Upstream at $Target does not carry: $($missing -join ', '). It has reorganised the " +
               "tree, so update `$Manifest in this script. Nothing has been modified.")
    }

    Write-Host "Copying $($Manifest.Count) files into src/ ..."
    foreach ($name in $Manifest) {
        [System.IO.File]::Copy((Join-Path $ClonePath $name), (Join-Path $SrcDir $name), $true)
    }
    # Upstream's licence text travels with the sources it covers. README.md cites it as the licence
    # of record, so refreshing the tree without it would leave that citation quietly ageing.
    [System.IO.File]::Copy((Join-Path $ClonePath 'LICENSE'), $LicensePath, $true)
    # Anything left in src/ that the manifest does not name is a leftover from an older layout.
    $stale = @(Get-ChildItem -LiteralPath $SrcDir -File | Where-Object { $Manifest -notcontains $_.Name })
    if ($stale.Count -gt 0) {
        Write-Warning "src/ holds files the manifest does not name: $($stale.Name -join ', ')"
    }

    Write-Host "Updating the recorded pin ..."
    $newReadme = $readme -replace '\*\*v[0-9][^*]*\*\* \(commit `[0-9a-f]{40}`\)', "**v$Version** (commit ``$targetSha``)"
    [System.IO.File]::WriteAllText($ReadmePath, $newReadme)

    # `[^\r\n]*` rather than `.*`: in .NET `.` matches \r, so `^version=.*$` would swallow the
    # CR and silently rewrite a CRLF file to LF.
    $props = [System.IO.File]::ReadAllText($PropsPath)
    $newProps = $props -replace '(?m)^version=[^\r\n]*', "version=$Version"
    if ($newProps -notmatch "(?m)^version=$([regex]::Escape($Version))\s*$") {
        throw "Could not update version= in $PropsPath"
    }
    [System.IO.File]::WriteAllText($PropsPath, $newProps)

    Write-Host ""
    Write-Host "Done. Vendored v$Version ($targetSha)." -ForegroundColor Green
    Write-Host "Review with: git -C `"$RepoRoot`" diff --stat firmware/quickjs-ng"
    Write-Host "Re-running against the pinned SHA should leave the tree unchanged."
} finally {
    if ($KeepWorkspace) {
        Write-Host "Workspace kept at $Workspace"
    } elseif (Test-Path -LiteralPath $Workspace) {
        Remove-Item -LiteralPath $Workspace -Recurse -Force -ErrorAction SilentlyContinue
    }
}
