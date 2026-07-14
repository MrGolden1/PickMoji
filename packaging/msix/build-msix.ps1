<#
.SYNOPSIS
    Build (and optionally sign) a PickMoji MSIX package for sideloading.

.DESCRIPTION
    Stages the deployed application from dist/ together with AppxManifest.xml and
    the logo assets, packs it into an .msix with makeappx.exe, and signs it with
    signtool.exe. If no signing certificate matching the manifest Publisher exists,
    a self-signed one is created in the CurrentUser store and its public .cer is
    exported so the package can be trusted on this machine.

.PREREQUISITES
    - Windows 10 SDK (makeappx.exe / signtool.exe).
    - The app already built and deployed to dist/ (see the repo README).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File packaging\msix\build-msix.ps1

.EXAMPLE
    # Just build the unsigned package (e.g. to sign with a real cert later):
    powershell -ExecutionPolicy Bypass -File packaging\msix\build-msix.ps1 -SkipSign
#>
[CmdletBinding()]
param(
    [string]$Version   = "1.0.0.0",
    [string]$Publisher = "CN=PickMoji",
    [string]$Dist,
    [string]$OutDir,
    [switch]$SkipSign
)

$ErrorActionPreference = "Stop"

$scriptDir = $PSScriptRoot
$repoRoot  = (Resolve-Path (Join-Path $scriptDir "..\..")).Path
if (-not $Dist)   { $Dist   = Join-Path $repoRoot "dist" }
if (-not $OutDir) { $OutDir = Join-Path $repoRoot "build-msix" }

$manifestSrc = Join-Path $scriptDir "AppxManifest.xml"
$assetsSrc   = Join-Path $scriptDir "Assets"
$exeRelative = "bin\PickMoji.exe"

function Find-SdkTool([string]$name) {
    $candidates = @()
    foreach ($base in @("${env:ProgramFiles(x86)}\Windows Kits\10\bin",
                        "${env:ProgramFiles}\Windows Kits\10\bin")) {
        if (Test-Path $base) {
            $candidates += Get-ChildItem -Path $base -Recurse -Filter $name -ErrorAction SilentlyContinue |
                Where-Object { $_.FullName -match "\\x64\\" }
        }
    }
    $tool = $candidates | Sort-Object FullName -Descending | Select-Object -First 1
    if (-not $tool) { throw "Could not find $name. Install the Windows 10/11 SDK." }
    return $tool.FullName
}

# --- Validate the deployed app ---------------------------------------------
$exeFull = Join-Path $Dist $exeRelative
if (-not (Test-Path $exeFull)) {
    throw "Missing $exeFull. Build and deploy first:`n" +
          "  cmake --build build-release`n" +
          "  cmake --install build-release --prefix dist"
}

$makeappx = Find-SdkTool "makeappx.exe"
Write-Host "makeappx : $makeappx"

# --- Stage the package layout ----------------------------------------------
$stage = Join-Path $OutDir "stage"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force -Path $stage | Out-Null

Copy-Item -Path (Join-Path $Dist "*") -Destination $stage -Recurse -Force
Copy-Item -Path $assetsSrc -Destination (Join-Path $stage "Assets") -Recurse -Force

# Write the manifest with the requested version/publisher so it always matches
# the certificate we sign with.
[xml]$manifest = Get-Content $manifestSrc
$manifest.Package.Identity.Version   = $Version
$manifest.Package.Identity.Publisher = $Publisher
$manifest.Save((Join-Path $stage "AppxManifest.xml"))

# --- Pack -------------------------------------------------------------------
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$msix = Join-Path $OutDir "PickMoji_$Version.msix"
& $makeappx pack /o /d $stage /p $msix
if ($LASTEXITCODE -ne 0) { throw "makeappx failed ($LASTEXITCODE)." }
Write-Host "Packed  : $msix" -ForegroundColor Green

if ($SkipSign) {
    Write-Host "`nUnsigned package created. Sign it before installing." -ForegroundColor Yellow
    return
}

# --- Ensure a signing certificate ------------------------------------------
$signtool = Find-SdkTool "signtool.exe"
Write-Host "signtool : $signtool"

$cert = Get-ChildItem Cert:\CurrentUser\My |
    Where-Object { $_.Subject -eq $Publisher -and $_.HasPrivateKey } |
    Sort-Object NotAfter -Descending | Select-Object -First 1

if (-not $cert) {
    Write-Host "Creating self-signed certificate $Publisher ..." -ForegroundColor Cyan
    $cert = New-SelfSignedCertificate `
        -Type Custom -Subject $Publisher `
        -KeyUsage DigitalSignature -FriendlyName "PickMoji (self-signed)" `
        -CertStoreLocation "Cert:\CurrentUser\My" `
        -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.3", "2.5.29.19={text}")
}

$cerPath = Join-Path $OutDir "PickMoji.cer"
Export-Certificate -Cert $cert -FilePath $cerPath | Out-Null

& $signtool sign /fd SHA256 /sha1 $cert.Thumbprint $msix
if ($LASTEXITCODE -ne 0) { throw "signtool failed ($LASTEXITCODE)." }
Write-Host "Signed  : $msix" -ForegroundColor Green

Write-Host @"

Next steps to install locally
-----------------------------
1. Trust the certificate (one time, needs an elevated shell):
     Import-Certificate -FilePath "$cerPath" ``
       -CertStoreLocation Cert:\LocalMachine\TrustedPeople
2. Install the package:
     Add-AppxPackage "$msix"
   (or just double-click the .msix in Explorer.)

For Store / production, sign with a real code-signing certificate and set the
matching Publisher/Identity in AppxManifest.xml instead of the self-signed CN.
"@ -ForegroundColor Gray
