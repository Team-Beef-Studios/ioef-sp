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
# NOTE: the run-*.bat launchers are deliberately NOT packaged -- the settings
# they used to pass on the command line are baked into the distribution's
# autoexec.cfg below, so users just run the exe.  (The .bat files remain in the
# dev build dir for flat/headless testing.)
$optionalRoot   = @()
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

# --- VR asset pk3 ------------------------------------------------------------
# Non-retail art the VR code needs (currently the selector-wheel save/load
# icons).  Built from the SAME source tree the Android APK packs
# (android/z_vr_assets_base), so the two platforms can't drift.  The 'z_' prefix
# makes the engine load it last, on top of the retail paks.
$vrAssetSrc = Join-Path $scriptDir 'android\z_vr_assets_base'
if (Test-Path $vrAssetSrc) {
    $vrPk3 = Join-Path $stageBaseEF 'z_vr_assets_base.pk3'
    Compress-Archive -Path (Join-Path $vrAssetSrc '*') -DestinationPath $vrPk3 -CompressionLevel Optimal
    Write-Host '  + baseEF/z_vr_assets_base.pk3 (VR art)'
} else {
    Write-Warning "VR asset source not found: $vrAssetSrc -- wheel icons will be missing."
}

# Safety net: make absolutely sure no RETAIL game data slipped in.  Our own
# z_vr_assets_base.pk3 (built just above) is the one pk3 that belongs here.
$strays = Get-ChildItem -Path $stage -Recurse -Include '*.pk3' -ErrorAction SilentlyContinue |
          Where-Object { $_.Name -ne 'z_vr_assets_base.pk3' }
if ($strays) { $strays | Remove-Item -Force; Write-Warning "Removed stray .pk3 file(s) from package." }

# --- Bake the VR launch defaults into the packaged autoexec.cfg -------------
# With no run-*.bat in the package, the settings the launchers used to pass on
# the command line must be the engine's defaults.  Append them to the staged
# autoexec.cfg (appended LAST so they win over any dev-build values):
#   vr_enable 1        -> boot straight into VR/OpenXR (code default is already 1)
#   vr_supersample 1.1 -> per-eye render scale (run-vr.bat's normal-mode value;
#                         the code default is 1.0)
#   r_fullscreen 0     -> the desktop mirror is a window (code default is 1); the
#                         headset itself is the real view
# There is deliberately no "+map" here -- launching lands on the SP main menu so
# the user can start a New Game, Load a save, or change Options.
# Also re-asserts the engine's tuned texture/LOD defaults (anisotropic filtering,
# picmip, texture filtering mode, gamma, LOD curve/bias, subdivisions) here, so a
# stale q3config.cfg/autoexec.cfg left over in the dev build dir can't shadow the
# compiled-in defaults in the packaged distribution.
$autoexecPath = Join-Path $stageBaseEF 'autoexec.cfg'
$vrDefaults = @"

// ---- PCVR distribution launch defaults (added by package-pcvr.ps1) ----
// These replace the old run-vr.bat command-line settings so the packaged
// ioquake3.x86_64.exe launches straight into VR with no launcher script.
// To run flat (a normal desktop window) instead, set vr_enable 0 here.
seta vr_enable 1
seta vr_supersample 1.1
seta r_fullscreen 0

// ---- Tuned texture/LOD defaults (added by package-pcvr.ps1) ----
seta r_ext_texture_filter_anisotropic 1
seta r_ext_max_anisotropy 2
seta r_textureMode GL_LINEAR_MIPMAP_LINEAR
seta r_gamma 0.810345
seta r_picmip 0
seta r_lodCurveError 500
seta r_subdivisions 1
seta r_lodbias -2
"@
if (Test-Path $autoexecPath) {
    Add-Content -Path $autoexecPath -Value $vrDefaults -Encoding ASCII
    Write-Host "  ~ autoexec.cfg (appended VR launch defaults)"
} else {
    # No dev autoexec was staged -- write a self-contained one so the packaged
    # build still boots SP in VR (misses dev tuning like sv_fps/r_clear, but VR
    # and SP will work).
    Write-Warning "No autoexec.cfg found in build; generating a minimal one for the package."
    $minimal = "// PCVR distribution autoexec (generated by package-pcvr.ps1)`r`nseta sp_game 1`r`nseta sv_fps 20`r`nseta r_clear 1" + $vrDefaults
    Set-Content -Path $autoexecPath -Value $minimal -Encoding ASCII
    Write-Host "  + autoexec.cfg (generated with SP + VR launch defaults)"
}

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
2. Double-click ioquake3.x86_64.exe.  It launches straight into VR and lands on
   the main menu -- start a New Game or Load a save from there.

Optional tweaks (edit baseEF\autoexec.cfg):
  * vr_enable 0        run flat in a normal desktop window (no VR)
  * vr_supersample 1.0 lower for more speed, higher (e.g. 1.25) for sharper eyes
  * vr_worldscale 32   world scale / stereo strength (higher = smaller world)

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
