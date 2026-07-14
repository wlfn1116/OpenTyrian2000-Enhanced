<#
  Build OpenTyrian 2000 for the Sony PS Vita  ->  vita\build\OpenTyrian2000.vpk

  Usage (from PowerShell, in the repo root or anywhere):
      powershell -ExecutionPolicy Bypass -File vita\build.ps1
      powershell -ExecutionPolicy Bypass -File vita\build.ps1 -Clean

  Requirements:
    * VitaSDK, with the sdl2 package installed (vdpm sdl2). Set $env:VITASDK or it
      defaults to D:\vitasdk.
    * A NATIVE Windows CMake (>= 3.16) and Ninja. The MSYS/devkitPro cmake on PATH does
      NOT work here (it hands the native Ninja POSIX /d/... paths it can't resolve), so we
      locate a native cmake explicitly. By default we use the CMake+Ninja that ship inside
      Visual Studio (found via vswhere) -- guaranteed present since rebuild-all.bat's PC
      target needs VS -- falling back to a standalone CMake or a `pip install cmake`, and to
      $VITASDK\bin\ninja.exe for Ninja. Override either with $env:CMAKE_EXE / $env:NINJA_EXE.

  Why PowerShell and not the MSYS build.sh: the Windows-native cmake+ninja must be driven with
  native (D:\...) paths; running them under MSYS bash mangles those paths. PowerShell drives
  them cleanly, and Copy-Item/robocopy/cmake -E tar handle the shell-hostile data filenames
  (shapes).dat, newsh%.shp, ...) without the per-file arg mangling that breaks vita-pack-vpk.
#>
param([switch]$Clean)

$ErrorActionPreference = 'Stop'

# --- paths -------------------------------------------------------------------
$Root  = $PSScriptRoot                                   # ...\vita
$Repo  = Split-Path $Root -Parent
$Build = Join-Path $Root  'build'
$Stage = Join-Path $Build 'vpk'
$Data  = Join-Path $Repo  'data'
$Vpk   = Join-Path $Build 'OpenTyrian2000.vpk'

$AppName = 'OpenTyrian 2000'
$TitleId = 'OTYR20000'
$AppVer  = '01.00'

# Delete a file/dir, tolerating a transient Windows lock (AV scan, a just-closed build handle).
function Remove-Robust([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return }
    for ($i = 0; $i -lt 5; $i++) {
        try { Remove-Item -LiteralPath $path -Recurse -Force -ErrorAction Stop; return }
        catch { Start-Sleep -Milliseconds 400 }
    }
    Remove-Item -LiteralPath $path -Recurse -Force   # final attempt; let it throw if truly stuck
}

# VitaSDK (forward-slash form -- cmake and its toolchain like that).
$Vitasdk = if ($env:VITASDK) { $env:VITASDK } else { 'D:/vitasdk' }
$Vitasdk = $Vitasdk -replace '\\', '/'
$env:VITASDK = $Vitasdk

if ($Clean) {
    Remove-Robust $Build
    Write-Host "cleaned $Build"
    return
}

# --- locate a native cmake + ninja -------------------------------------------
# The native CMake+Ninja must NOT be the MSYS/devkitPro ones on PATH (they hand the native
# Ninja POSIX /d/... paths it can't resolve). Search order: an explicit $env:CMAKE_EXE /
# $env:NINJA_EXE override, a standalone CMake install, the CMake+Ninja that ship INSIDE
# Visual Studio (guaranteed present -- rebuild-all.bat's PC target needs VS), then a
# `pip install cmake` in the user's Python. For Ninja, VitaSDK's own copy stays preferred.
function Find-Tool([string]$envVar, [string[]]$candidates, [string]$name) {
    if ((Test-Path Env:$envVar) -and (Test-Path (Get-Item Env:$envVar).Value)) {
        return (Get-Item (Get-Item Env:$envVar).Value).FullName
    }
    foreach ($c in $candidates) {
        if ([string]::IsNullOrWhiteSpace($c)) { continue }
        $hit = Get-Item $c -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($hit) { return $hit.FullName }
    }
    throw "Could not find $name. Set `$env:$envVar to its full path."
}

# Visual Studio bundles CMake + Ninja under Common7\IDE\CommonExtensions\Microsoft\CMake.
# Ask vswhere where VS is (covers any edition/year), then fall back to known static paths.
# Returns that ...\Microsoft\CMake dir (which holds CMake\bin\cmake.exe and Ninja\ninja.exe).
function Find-VSCMakeDir {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $rel = 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
        $hit = & $vswhere -latest -prerelease -products * -find $rel 2>$null | Select-Object -First 1
        if ($hit -and (Test-Path $hit)) { return (Split-Path (Split-Path (Split-Path $hit))) }
    }
    foreach ($base in @(
        'C:\Program Files\Microsoft Visual Studio\18\Community',
        'C:\Program Files\Microsoft Visual Studio\2022\Community',
        'C:\Program Files\Microsoft Visual Studio\2022\Professional',
        'C:\Program Files\Microsoft Visual Studio\2022\Enterprise'
    )) {
        $dir = Join-Path $base 'Common7\IDE\CommonExtensions\Microsoft\CMake'
        if (Test-Path (Join-Path $dir 'CMake\bin\cmake.exe')) { return $dir }
    }
    return $null
}
$VsCMakeDir = Find-VSCMakeDir
$VsCmake = if ($VsCMakeDir) { Join-Path $VsCMakeDir 'CMake\bin\cmake.exe' } else { $null }
$VsNinja = if ($VsCMakeDir) { Join-Path $VsCMakeDir 'Ninja\ninja.exe' }   else { $null }

$Cmake = Find-Tool 'CMAKE_EXE' @(
    'C:\Program Files\CMake\bin\cmake.exe',
    $VsCmake,
    "$env:LOCALAPPDATA\Programs\Python\Python3*\Lib\site-packages\cmake\data\bin\cmake.exe",
    "$env:APPDATA\Python\Python3*\site-packages\cmake\data\bin\cmake.exe"
) 'a native Windows CMake'

$Ninja = Find-Tool 'NINJA_EXE' @(
    "$Vitasdk/bin/ninja.exe",
    $VsNinja,
    'C:\Program Files\CMake\bin\ninja.exe'
) 'ninja'

Write-Host "cmake:  $Cmake"
Write-Host "ninja:  $Ninja"
Write-Host "vitasdk: $Vitasdk"

# --- 1. LiveArea assets (8-bit indexed PNGs) ---------------------------------
if (-not (Test-Path (Join-Path $Root 'sce_sys\icon0.png'))) {
    Write-Host '== generating LiveArea assets =='
    & powershell -ExecutionPolicy Bypass -File (Join-Path $Root 'make_livearea.ps1')
}

# --- 2. Configure + compile -> eboot.bin -------------------------------------
Write-Host '== cmake configure =='
& $Cmake -S $Root -B $Build -G Ninja `
    -DCMAKE_MAKE_PROGRAM="$Ninja" `
    -DCMAKE_TOOLCHAIN_FILE="$Vitasdk/share/vita.toolchain.cmake"
if ($LASTEXITCODE) { throw "cmake configure failed ($LASTEXITCODE)" }

Write-Host '== cmake build =='
& $Cmake --build $Build
if ($LASTEXITCODE) { throw "cmake build failed ($LASTEXITCODE)" }

# --- 3. param.sfo ------------------------------------------------------------
Write-Host '== param.sfo =='
& "$Vitasdk/bin/vita-mksfoex.exe" -s APP_VER=$AppVer -s TITLE_ID=$TitleId -s CATEGORY=gd `
    $AppName (Join-Path $Build 'param.sfo')
if ($LASTEXITCODE) { throw "vita-mksfoex failed ($LASTEXITCODE)" }

# --- 4. Stage the VPK tree ---------------------------------------------------
Write-Host '== staging =='
Remove-Robust $Stage
New-Item -ItemType Directory -Force (Join-Path $Stage 'sce_sys\livearea\contents') | Out-Null
New-Item -ItemType Directory -Force (Join-Path $Stage 'data') | Out-Null

Copy-Item (Join-Path $Build 'eboot.bin')                    (Join-Path $Stage 'eboot.bin')
Copy-Item (Join-Path $Build 'param.sfo')                    (Join-Path $Stage 'sce_sys\param.sfo')
Copy-Item (Join-Path $Root  'sce_sys\icon0.png')            (Join-Path $Stage 'sce_sys\icon0.png')
Copy-Item (Join-Path $Root  'sce_sys\pic0.png')             (Join-Path $Stage 'sce_sys\pic0.png')
Copy-Item (Join-Path $Root  'sce_sys\livearea\contents\*')  (Join-Path $Stage 'sce_sys\livearea\contents')

# The freeware Tyrian data. That data\ folder is a full Windows distro; bundle only the game
# data, excluding the Windows/DOS binaries and desktop config/saves (same list as the Switch
# romfs), plus the ~55 MB WeedsGM3.sf2 soundfont, which only feeds the optional FluidSynth MIDI
# path -- off here (WITH_MIDI is Windows-x64-only), so it is pure dead weight on the Vita.
# robocopy copies the shell-hostile filenames verbatim and returns <8 on success.
robocopy $Data (Join-Path $Stage 'data') /E `
    /XF *.exe *.dll *.pif *.ico *.box *.ovl *.cfg *.sav *.txt *.sf2 `
    /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE)" }

# --- 5. Pack the tree into a .vpk (a zip) ------------------------------------
Write-Host '== packing VPK =='
Remove-Robust $Vpk
Push-Location $Stage
try { & $Cmake -E tar cf $Vpk --format=zip . }
finally { Pop-Location }
if ($LASTEXITCODE) { throw "vpk packing failed ($LASTEXITCODE)" }

$size = [math]::Round((Get-Item $Vpk).Length / 1MB, 1)
Write-Host ''
Write-Host "built: $Vpk  ($size MB)"
