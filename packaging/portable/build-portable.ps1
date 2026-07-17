<#
.SYNOPSIS
    Build a clean, portable PickMoji package.

.DESCRIPTION
    Produces one tidy folder — PickMoji.exe at its root, the Qt runtime DLLs
    beside it, and the plugin folders (platforms/styles/tls) beside it — then
    zips it as PickMoji-<version>-win64.zip. No installer, no self-extracting
    wrapper, no packed/virtualized DLLs: the user unzips and runs PickMoji.exe.

    Trims what a QWidgets app never loads (the DirectX shader compilers) and
    ships the VC++ runtime DLLs directly rather than the redistributable
    installer, so the folder is self-contained.

.NOTES
    Build the Release target first (cmake --build build-release).
#>
[CmdletBinding()]
param(
    [string]$BuildDir = "build-release",
    [string]$QtBin    = "E:/Qt/6.8.2/msvc2022_64/bin",
    [string]$VcVars   = "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Auxiliary/Build/vcvars64.bat"
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
$exe  = Join-Path $repo "$BuildDir/PickMoji.exe"
if (-not (Test-Path $exe))   { throw "Not found: $exe  (build the Release target first)." }
$windeploy = Join-Path $QtBin "windeployqt.exe"
if (-not (Test-Path $windeploy)) { throw "windeployqt not found: $windeploy  (pass -QtBin)." }
if (-not (Test-Path $VcVars))    { throw "vcvars64.bat not found: $VcVars  (pass -VcVars)." }

# Version from CMakeLists: project(PickMoji VERSION x.y.z ...)
$cml = Get-Content (Join-Path $repo "CMakeLists.txt") -Raw
if ($cml -notmatch 'project\(PickMoji\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)') {
    throw "Could not read 'project(PickMoji VERSION ...)' from CMakeLists.txt."
}
$version = $Matches[1]

$stageRoot = Join-Path $repo "build-portable"
$stage     = Join-Path $stageRoot "PickMoji"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force -Path $stage | Out-Null
Copy-Item $exe (Join-Path $stage "PickMoji.exe")

# Deploy the Qt runtime next to the exe (windeployqt's default layout is already
# clean: DLLs beside the exe, plugins in platforms/ styles/ tls/). Run inside
# vcvars so it bundles the actual VC++ runtime DLLs, not the redist installer.
$deploy = "`"$windeploy`" `"$stage\PickMoji.exe`" --release --no-translations " +
          "--no-opengl-sw --no-system-d3d-compiler --compiler-runtime " +
          "--skip-plugin-types generic,iconengines,imageformats,networkinformation"
& cmd /c "call `"$VcVars`" >nul 2>&1 && $deploy"
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed (exit $LASTEXITCODE)." }

# Drop the DXC shader compilers a QWidgets app never touches.
foreach ($junk in "dxcompiler.dll", "dxil.dll") {
    Remove-Item -Force (Join-Path $stage $junk) -ErrorAction SilentlyContinue
}

# windeployqt often ships vc_redist.x64.exe (the installer) rather than the CRT
# DLLs. A portable folder should carry the DLLs directly. The BuildTools install
# has no redist folder, so take the (redistributable) runtime DLLs from System32,
# then drop the installer.
if (-not (Test-Path (Join-Path $stage "msvcp140.dll"))) {
    $sys = Join-Path $env:WINDIR "System32"
    $crtDlls = @("vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll",
                 "msvcp140_1.dll", "msvcp140_2.dll", "msvcp140_atomic_wait.dll",
                 "msvcp140_codecvt_ids.dll", "concrt140.dll")
    foreach ($dll in $crtDlls) {
        $src = Join-Path $sys $dll
        if (Test-Path $src) { Copy-Item $src $stage }
    }
}
Remove-Item -Force (Join-Path $stage "vc_redist.x64.exe") -ErrorAction SilentlyContinue

# Sanity check: the essentials must be present after trimming.
foreach ($need in "PickMoji.exe", "Qt6Core.dll", "Qt6Network.dll",
                  "msvcp140.dll", "platforms/qwindows.dll", "tls/qschannelbackend.dll") {
    if (-not (Test-Path (Join-Path $stage $need))) {
        throw "Missing from package: $need  (deployment incomplete)."
    }
}

# App data + notices.
Copy-Item (Join-Path $repo "keywords") (Join-Path $stage "keywords") -Recurse
foreach ($doc in "LICENSE", "THIRD_PARTY_NOTICES.md", "TWEMOJI-LICENSE-GRAPHICS.txt") {
    Copy-Item (Join-Path $repo $doc) $stage
}

# Zip the folder itself so it extracts as PickMoji/.
$zip = Join-Path $stageRoot "PickMoji-$version-win64.zip"
Remove-Item -Force $zip -ErrorAction SilentlyContinue
Compress-Archive -Path $stage -DestinationPath $zip

$size = [math]::Round((Get-Item $zip).Length / 1MB, 1)
Write-Host ""
Write-Host "Portable package : $zip  (${size} MB)"
Write-Host "Staging folder   : $stage"
