<#
.SYNOPSIS
    Package the PCVR (Windows x86_64) build of the Elite Force VR engine into a
    distributable .zip containing ONLY what's needed to run it.

.DESCRIPTION
    Collects the engine exe, the runtime DLLs (renderer, OpenXR loader, SDL2,
    MinGW runtime), the SP game/UI DLLs and the config from baseEF/, plus the
    run-*.bat launchers, into a single versioned archive.

    Deliberately EXCLUDES the retail game data (baseEF/*.pk3) -- the player must
    supply their own copy of Elite Force. A README-FIRST.txt explaining this is
    generated into the package.

    The version is read from the VR-port version header (EFVR_VERSION_NUMBER,
    which Q3_VERSION resolves to) in the sibling Elite-Force-VR repo, so the zip
    is named e.g. EliteForceVR-PCVR-v0.0.8.zip.

.PARAMETER BuildDir
    The engine build directory to package. Defaults to the release MinGW64 build.

.PARAMETER VersionHeader
    Path to stv_version.h. Defaults to the sibling Elite-Force-VR checkout.

.PARAMETER OutDir
    Where the .zip is written. Defaults to <repo>\dist.

.PARAMETER Version
    Override the version string instead of reading it from the header.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\package-pcvr.ps1
#>
[CmdletBinding()]
param(
    [string]$BuildDir      = '',
    [string]$VersionHeader = '',
    [string]$OutDir        = '',
    [string]$Version       = ''
)

$ErrorActionPreference = 'Stop'

function Fail($msg) { Write-Error $msg; exit 1 }

# Resolve the script's own directory robustly ($PSScriptRoot isn't always set in
# param-default scope depending on how the script is invoked).
$scriptDir = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
if (-not $BuildDir)      { $BuildDir      = Join-Path $scriptDir 'build\release-mingw64-x86_64' }
if (-not $VersionHeader) { $VersionHeader = Join-Path $scriptDir '..\Elite-Force-VR\qcommon\stv_version.h' }
if (-not $OutDir)        { $OutDir        = Join-Path $scriptDir 'dist' }

# --- Resolve the version ---------------------------------------------------
if (-not $Version) {
    if (-not (Test-Path $VersionHeader)) {
        Fail "Version header not found: $VersionHeader`nPass -Version x.y.z or -VersionHeader <path> to stv_version.h."
    }
    $headerText = Get-Content -Raw $VersionHeader
    $m = [regex]::Match($headerText, '#define\s+EFVR_VERSION_NUMBER\s+"([^"]+)"')
    if (-not $m.Success) {
        Fail "Could not find EFVR_VERSION_NUMBER in $VersionHeader. Pass -Version x.y.z instead."
    }
    $Version = $m.Groups[1].Value
}
Write-Host "Version: $Version (Elite Force VR v$Version)" -ForegroundColor Cyan

# --- File manifest ---------------------------------------------------------
# Required: packaging fails if any of these are missing.
$requiredRoot = @(
    'ioquake3.x86_64.exe',
    'renderer_opengl1_x86_64.dll',
    'libopenxr_loader.dll',
    'SDL264.dll',
    'libgcc_s_seh-1.dll',
    'libstdc++-6.dll',
    'libwinpthread-1.dll'
)
$requiredBaseEF = @(
    'efgamex86_64.dll',
    'efuix86_64.dll'
)
# Optional: included if present, a warning if not (never fatal).
$optionalRoot   = @('run-vr.bat', 'run-flat.bat')
$optionalBaseEF = @('autoexec.cfg')

if (-not (Test-Path $BuildDir)) { Fail "Build directory not found: $BuildDir" }

# --- Stage -----------------------------------------------------------------
$pkgName = "EliteForceVR-PCVR-v$Version"
$stageRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("efvr-pkg-" + [System.Guid]::NewGuid().ToString('N'))
$stage = Join-Path $stageRoot $pkgName
$stageBaseEF = Join-Path $stage 'baseEF'
New-Item -ItemType Directory -Force -Path $stageBaseEF | Out-Null

function Stage-File($srcDir, $name, $destDir, $required) {
    $src = Join-Path $srcDir $name
    if (Test-Path $src) {
        Copy-Item $src -Destination $destDir -Force
        Write-Host ("  + {0}" -f $name)
        return $true
    }
    if ($required) { Fail "Required file missing from build: $src" }
    Write-Warning "  (skipped, not found) $name"
    return $false
}

$baseEFDir = Join-Path $BuildDir 'baseEF'
Write-Host "Staging files:" -ForegroundColor Cyan
foreach ($f in $requiredRoot)   { Stage-File $BuildDir   $f $stage       $true  | Out-Null }
foreach ($f in $optionalRoot)   { Stage-File $BuildDir   $f $stage       $false | Out-Null }
foreach ($f in $requiredBaseEF) { Stage-File $baseEFDir  $f $stageBaseEF $true  | Out-Null }
foreach ($f in $optionalBaseEF) { Stage-File $baseEFDir  $f $stageBaseEF $false | Out-Null }

# Safety net: make absolutely sure no game data slipped in.
$strays = Get-ChildItem -Path $stage -Recurse -Include '*.pk3' -ErrorAction SilentlyContinue
if ($strays) { $strays | Remove-Item -Force; Write-Warning "Removed stray .pk3 file(s) from package." }

# --- README inside the package --------------------------------------------
$readme = @"
Elite Force VR (PCVR) v$Version
================================

A virtual-reality port of Star Trek: Voyager - Elite Force single-player,
built on the ioquake3-derived ioEF engine (OpenXR).

REQUIRED: YOUR OWN GAME DATA
----------------------------
This package does NOT include the retail game data. You must own Elite Force
and copy its .pk3 files into the baseEF\ folder next to this README:

    baseEF\pak0.pk3
    baseEF\pak1.pk3
    baseEF\pak2.pk3
    baseEF\pak3.pk3
    baseEF\playermaps.pk3

(Copy them from your existing Elite Force install / the companion engine build.)

RUNNING
-------
1. Start your OpenXR runtime first (SteamVR, or the Oculus/Meta PC app) with the
   headset connected.
2. Double-click run-vr.bat   (or run-flat.bat for a non-VR window).

The engine (this package) is licensed under the GNU GPL v2; the Elite Force SP
game modules (efgamex86_64.dll / efuix86_64.dll) are under the proprietary STEF
Game Source License. Game assets are the property of their rights holders and
are not distributed here.
"@
Set-Content -Path (Join-Path $stage 'README-FIRST.txt') -Value $readme -Encoding ASCII
Write-Host "  + README-FIRST.txt"

# --- Zip -------------------------------------------------------------------
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$zipPath = Join-Path $OutDir "$pkgName.zip"
if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
Compress-Archive -Path $stage -DestinationPath $zipPath -CompressionLevel Optimal

Remove-Item -Recurse -Force $stageRoot

# --- Summary ---------------------------------------------------------------
$zip = Get-Item $zipPath
$sizeMB = [math]::Round($zip.Length / 1MB, 2)
Write-Host ""
Write-Host "Packaged: $($zip.FullName)" -ForegroundColor Green
Write-Host "Size:     $sizeMB MB"
