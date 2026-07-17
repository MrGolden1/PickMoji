<#
.SYNOPSIS
    Build the PickMoji per-user installer (Setup exe) with Inno Setup.

.DESCRIPTION
    Refreshes the clean staging folder (via build-portable.ps1) and compiles it
    into build-portable/PickMoji-<version>-Setup.exe. Requires Inno Setup 6
    (ISCC.exe); install it with:  winget install -e --id JRSoftware.InnoSetup

.NOTES
    Build the Release target first (cmake --build build-release).
#>
[CmdletBinding()]
param(
    [switch]$SkipStaging,   # reuse the existing build-portable/PickMoji staging
    [string]$Iscc = ""      # explicit path to ISCC.exe, if not auto-found
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path

if (-not $Iscc) {
    $candidates = @(
        "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
        "$env:ProgramFiles\Inno Setup 6\ISCC.exe",
        "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
    )
    $Iscc = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $Iscc) {
        $cmd = Get-Command ISCC.exe -ErrorAction SilentlyContinue
        if ($cmd) { $Iscc = $cmd.Source }
    }
}
if (-not $Iscc) {
    throw "Inno Setup (ISCC.exe) not found. Install it: winget install -e --id JRSoftware.InnoSetup"
}

$staging = Join-Path $repo "build-portable/PickMoji"
if (-not $SkipStaging -or -not (Test-Path $staging)) {
    & powershell -NoProfile -ExecutionPolicy Bypass `
        -File (Join-Path $repo "packaging/portable/build-portable.ps1")
    if ($LASTEXITCODE -ne 0) { throw "Portable staging step failed." }
}

$cml = Get-Content (Join-Path $repo "CMakeLists.txt") -Raw
if ($cml -notmatch 'project\(PickMoji\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)') {
    throw "Could not read 'project(PickMoji VERSION ...)' from CMakeLists.txt."
}
$version = $Matches[1]
$outDir  = Join-Path $repo "build-portable"
$iss     = Join-Path $repo "packaging/installer/PickMoji.iss"

& $Iscc "/DAppVersion=$version" "/DStagingDir=$staging" "/DOutputDir=$outDir" $iss
if ($LASTEXITCODE -ne 0) { throw "ISCC failed (exit $LASTEXITCODE)." }

$setup = Join-Path $outDir "PickMoji-$version-Setup.exe"
$size = [math]::Round((Get-Item $setup).Length / 1MB, 1)
Write-Host ""
Write-Host "Installer: $setup  (${size} MB)"
