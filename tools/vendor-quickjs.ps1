<#
.SYNOPSIS
Re-vendor firmware/quickjs-ng/src/ from upstream QuickJS-ng, replaying the local patches.

.DESCRIPTION
Clones upstream into a scratch workspace under .temp/, checks out the currently pinned
baseline, replays firmware/quickjs-ng/patches/*.patch onto it as real commits, rebases
those commits onto the requested target, regenerates the patches against the new baseline,
copies the vendored file manifest into firmware/quickjs-ng/src/, and updates the pin
recorded in README.md and library.properties.

Rebase rather than `git apply` is deliberate: it gives a 3-way merge, so when upstream moves
the patched code you get real conflict markers to resolve instead of a flat rejection.

Run with the currently pinned SHA to verify reproducibility: `git diff` must come back empty.

.PARAMETER Target
Upstream tag or commit SHA to vendor, e.g. `v0.16.0` or a 40-char SHA.

.PARAMETER Version
Version string to record. Defaults to the tag `Target` resolves to, with any leading `v`
stripped. Required when the target is not tagged.

.PARAMETER KeepWorkspace
Leave the .temp/ workspace in place instead of deleting it. Use this to inspect conflict
markers after a failed rebase.

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
$PatchDir    = Join-Path $VendorDir 'patches'
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

# The pin recorded in README.md is the single source of truth for the current baseline.
$readme = Get-Content -Raw -LiteralPath $ReadmePath
if ($readme -notmatch '\*\*v(?<ver>[0-9][^*]*)\*\* \(commit `(?<sha>[0-9a-f]{40})`\)') {
    throw "Could not find the pin in $ReadmePath. Expected a line matching: **v<version>** (commit ``<40-char sha>``)"
}
$BaselineSha     = $Matches['sha']
$BaselineVersion = $Matches['ver']
Write-Host "Current pin: v$BaselineVersion ($BaselineSha)"

$patches = @(Get-ChildItem -LiteralPath $PatchDir -Filter '*.patch' | Sort-Object Name)
# Deliberate guard, not an invariant of the design: an empty patches/ almost always means they were
# deleted by accident. If upstream ever takes the Xtensa fixes the local delta legitimately becomes
# zero, and re-vendoring turns into a plain copy — delete this check when that day comes.
if ($patches.Count -eq 0) { throw "No patches found in $PatchDir" }
Write-Host "Local patches: $($patches.Count)"

# git am and git rebase both need a committer identity; fall back if the clone inherits none.
$userName  = Get-GitOutput -Arguments @('config', 'user.name')  -WorkingDirectory $RepoRoot
$userEmail = Get-GitOutput -Arguments @('config', 'user.email') -WorkingDirectory $RepoRoot
if (-not $userName)  { $userName  = 'vendor-quickjs' }
if (-not $userEmail) { $userEmail = 'vendor-quickjs@localhost' }
$idArgs = @('-c', "user.name=$userName", '-c', "user.email=$userEmail")

if (Test-Path -LiteralPath $Workspace) { Remove-Item -LiteralPath $Workspace -Recurse -Force }
New-Item -ItemType Directory -Force -Path $Workspace | Out-Null

try {
    Write-Host "Cloning $UpstreamUrl ..."
    Invoke-Git -Arguments @('clone', '--filter=blob:none', '--quiet', $UpstreamUrl, $ClonePath)

    Write-Host "Checking out baseline $BaselineSha ..."
    Invoke-Git -Arguments @('checkout', '--quiet', '-B', 'vendor-local', $BaselineSha) -WorkingDirectory $ClonePath

    # Resolve the target before doing any work, so a typo'd tag fails here rather than surfacing
    # later disguised as a rebase conflict.
    $targetSha = Get-GitOutput -Arguments @('rev-parse', '--verify', '--quiet', "$Target^{commit}") -WorkingDirectory $ClonePath
    if (-not $targetSha) { throw "Target '$Target' does not resolve to a commit in $UpstreamUrl" }

    # Feed the patches with LF endings whatever they look like on disk. This repo checks out
    # with core.autocrlf=true, so a tracked patch file can arrive CRLF; git am would then
    # apply CRLF-terminated lines into an LF file and the result would not match upstream.
    $amInput = Join-Path $Workspace 'am-input'
    New-Item -ItemType Directory -Force -Path $amInput | Out-Null
    foreach ($p in $patches) {
        $text = [System.IO.File]::ReadAllText($p.FullName) -replace "`r`n", "`n"
        [System.IO.File]::WriteAllText((Join-Path $amInput $p.Name), $text, (New-Object System.Text.UTF8Encoding($false)))
    }

    Write-Host "Replaying $($patches.Count) patch(es) onto the baseline ..."
    $amFiles = @(Get-ChildItem -LiteralPath $amInput -Filter '*.patch' | Sort-Object Name | ForEach-Object { $_.FullName })
    try {
        Invoke-Git -Arguments ($idArgs + @('am') + $amFiles) -WorkingDirectory $ClonePath
    } catch {
        & git -C $ClonePath am --abort 2>$null
        throw "git am failed: the patches do not apply to the pinned baseline $BaselineSha. $_"
    }

    # `--onto <target> <baseline>` rather than a plain `rebase <target>`: this replays exactly the
    # commits sitting on top of the baseline, whatever the ancestry between the two. A plain rebase
    # onto a target that is an *ancestor* of the baseline is a silent no-op ("up to date"), which
    # leaves upstream's own commits in the range and makes format-patch emit them as local patches.
    Write-Host "Rebasing local commits onto $Target ($targetSha) ..."
    try {
        Invoke-Git -Arguments ($idArgs + @('rebase', '--onto', $targetSha, $BaselineSha)) -WorkingDirectory $ClonePath
    } catch {
        $hint = if ($KeepWorkspace) { "Workspace kept at $ClonePath" } else { "Re-run with -KeepWorkspace to inspect the conflict" }
        & git -C $ClonePath rebase --abort 2>$null
        throw "Rebase onto $Target hit conflicts: upstream has moved the patched code and the patches need updating by hand. $hint. $_"
    }

    # Resolve the version to record.
    if (-not $Version) {
        $tag = Get-GitOutput -Arguments @('describe', '--tags', '--exact-match', $targetSha) -WorkingDirectory $ClonePath
        if (-not $tag) { throw "Target '$Target' is not an exact tag; pass -Version explicitly." }
        $Version = $tag -replace '^v', ''
    }
    Write-Host "Recording version $Version at $targetSha"

    # Check every manifest name resolves before anything on disk is touched. This is the failure
    # the explicit manifest is designed to have, so it must land cleanly: discovered mid-copy it
    # would leave src/ half old and half new, with patches/ already swapped underneath it.
    $missing = @(($Manifest + 'LICENSE') | Where-Object { -not (Test-Path -LiteralPath (Join-Path $ClonePath $_)) })
    if ($missing.Count -gt 0) {
        throw ("Upstream at $Target does not carry: $($missing -join ', '). It has reorganised the " +
               "tree, so update `$Manifest in this script. Nothing has been modified.")
    }

    Write-Host "Regenerating patches against the new baseline ..."
    # Generate into a scratch directory and swap only once the count checks out, so a surprise
    # here leaves patches/ exactly as it was instead of half-rewritten.
    $patchOut = Join-Path $Workspace 'patches-out'
    New-Item -ItemType Directory -Force -Path $patchOut | Out-Null
    Invoke-Git -Arguments @('format-patch', '--no-numbered', '--zero-commit', '--no-signature',
                            '-o', $patchOut, $targetSha) -WorkingDirectory $ClonePath
    $regenerated = @(Get-ChildItem -LiteralPath $patchOut -Filter '*.patch' | Sort-Object Name)
    if ($regenerated.Count -ne $patches.Count) {
        throw ("Expected $($patches.Count) regenerated patch(es), got $($regenerated.Count). " +
               "The rebase left a different set of commits on top of $Target than the patches it " +
               "started from. patches/ is untouched.")
    }
    # Enumerate and delete, rather than passing a wildcard: -LiteralPath would take `*.patch`
    # literally and -Path errors when nothing matches. Old patches must go, or a run whose
    # subject line changed would leave the previous file behind next to the new one.
    Get-ChildItem -LiteralPath $PatchDir -Filter '*.patch' | Remove-Item -Force
    foreach ($p in $regenerated) {
        [System.IO.File]::Copy($p.FullName, (Join-Path $PatchDir $p.Name), $true)
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
