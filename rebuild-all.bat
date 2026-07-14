@echo off
REM ====================================================================
REM  rebuild-all.bat
REM  Rebuild the OpenTyrian2000 widescreen fork for all three targets and
REM  collect the deliverables into .\build\ :
REM     1) PC      - MSVC x64 Release   -> opentyrian-x64-Release.exe (+ .pdb)
REM     2) Switch  - devkitA64 .nro     -> switch\opentyrian2000.nro
REM     3) Vita    - VitaSDK .vpk       -> vita\build\OpenTyrian2000.vpk
REM  Each is copied into .\build\ after a successful build (the originals
REM  stay where each toolchain writes them; the PC .exe must keep living
REM  next to data\ to run, so build\ is a COLLECTION of the deliverables).
REM
REM  Just run it (double-click, or from a terminal). Each target builds
REM  incrementally; a PASS/FAIL summary is printed at the end. One target
REM  failing does not stop the others.
REM
REM  Toolchain paths auto-resolve, but you can override them by setting
REM  any of these before running:
REM     MSBUILD_EXE      full path to MSBuild.exe
REM     DEVKITPRO_BASH   full path to devkitPro's msys2 bash.exe
REM     VITASDK          VitaSDK root (consumed by vita\build.ps1)
REM  Set NOPAUSE=1 to skip the "press a key" prompt at the end.
REM ====================================================================
setlocal EnableExtensions
title Rebuild OpenTyrian2000 - PC + Switch + Vita

REM Work from the repo root (this script's own folder), whatever the caller's cwd.
pushd "%~dp0"

if exist "visualc\opentyrian.vcxproj" goto :root_ok
echo ERROR: run this from the repo root - visualc\opentyrian.vcxproj not found.
popd
if not defined NOPAUSE pause
exit /b 1
:root_ok

echo ==================================================================
echo   OpenTyrian2000 : rebuild all targets  ^(PC + Switch + Vita^)
echo ==================================================================

REM ---- resolve toolchains -------------------------------------------
if not defined DEVKITPRO_BASH set "DEVKITPRO_BASH=D:\devkitPro\msys2\usr\bin\bash.exe"
if not defined MSBUILD_EXE call :find_msbuild

set "BUILD_DIR=%~dp0build"
set "PC_RESULT=skipped"
set "SWITCH_RESULT=skipped"
set "VITA_RESULT=skipped"

REM ================= 1/3  PC  (MSVC x64 Release) =====================
echo.
echo [1/3] PC  ^(MSVC x64 Release^) ...
if not exist "%MSBUILD_EXE%" goto :pc_nomsbuild
"%MSBUILD_EXE%" "visualc\opentyrian.vcxproj" -p:Configuration=Release -p:Platform=x64 -p:PlatformToolset=v145 -nologo -verbosity:minimal
if errorlevel 1 (set "PC_RESULT=FAILED") else (set "PC_RESULT=OK")
goto :build_switch
:pc_nomsbuild
echo        MSBuild not found: "%MSBUILD_EXE%"
echo        Set MSBUILD_EXE to your MSBuild.exe and re-run.
set "PC_RESULT=FAILED - MSBuild not found"

REM ================= 2/3  Switch  (.nro) =============================
:build_switch
echo.
echo [2/3] Switch  ^(.nro^) ...
if not exist "%DEVKITPRO_BASH%" goto :switch_nobash
pushd "switch"
REM Neutralize any ambient DEVKITPRO that isn't in msys form (e.g. a Windows-style
REM D:\devkitPro inherited from a parent shell) so build.sh defaults it to the
REM correct /opt/devkitpro msys mount. Harmless when DEVKITPRO is already unset.
set "DEVKITPRO="
"%DEVKITPRO_BASH%" build.sh
if errorlevel 1 (set "SWITCH_RESULT=FAILED") else (set "SWITCH_RESULT=OK")
popd
goto :build_vita
:switch_nobash
echo        devkitPro bash not found: "%DEVKITPRO_BASH%"
echo        Set DEVKITPRO_BASH to your msys2 bash.exe and re-run.
set "SWITCH_RESULT=FAILED - devkitPro not found"

REM ================= 3/3  Vita  (.vpk) ===============================
:build_vita
echo.
echo [3/3] Vita  ^(.vpk^) ...
powershell -NoProfile -ExecutionPolicy Bypass -File "vita\build.ps1"
if errorlevel 1 (set "VITA_RESULT=FAILED") else (set "VITA_RESULT=OK")

REM ============= collect deliverables into .\build\ ==================
echo.
echo Collecting artifacts into "%BUILD_DIR%" ...
mkdir "%BUILD_DIR%" 2>nul
if /I "%PC_RESULT%"=="OK"     call :collect     "opentyrian-x64-Release.exe"
if /I "%PC_RESULT%"=="OK"     call :collect_opt "opentyrian-x64-Release.pdb"
if /I "%SWITCH_RESULT%"=="OK" call :collect     "switch\opentyrian2000.nro"
if /I "%VITA_RESULT%"=="OK"   call :collect     "vita\build\OpenTyrian2000.vpk"

REM ================= summary =========================================
echo.
echo ==================================================================
echo   Build summary
echo     PC      :  %PC_RESULT%
echo     Switch  :  %SWITCH_RESULT%
echo     Vita    :  %VITA_RESULT%
echo   ----------------------------------------------------------------
echo   Artifacts collected in:  %BUILD_DIR%
echo ==================================================================

set "EXITCODE=0"
if /I not "%PC_RESULT%"=="OK" set "EXITCODE=1"
if /I not "%SWITCH_RESULT%"=="OK" set "EXITCODE=1"
if /I not "%VITA_RESULT%"=="OK" set "EXITCODE=1"

popd
echo.
if not defined NOPAUSE pause
exit /b %EXITCODE%

REM ------------------------------------------------------------------
REM  :collect      copy a required artifact into BUILD_DIR (warn if missing)
REM  :collect_opt  copy an optional artifact into BUILD_DIR (silent if missing)
REM  %~1 = source path relative to repo root; kept under its own name in build\.
REM ------------------------------------------------------------------
:collect
if not exist "%~1" goto :collect_missing
copy /y "%~1" "%BUILD_DIR%\%~nx1" >nul
if errorlevel 1 goto :collect_failed
echo        + %~nx1
goto :eof
:collect_missing
echo        WARNING: expected artifact not found: %~1
goto :eof
:collect_failed
echo        WARNING: failed to copy: %~1
goto :eof

:collect_opt
if not exist "%~1" goto :eof
copy /y "%~1" "%BUILD_DIR%\%~nx1" >nul
if not errorlevel 1 echo        + %~nx1
goto :eof

REM ------------------------------------------------------------------
REM  :find_msbuild  - locate MSBuild.exe (known VS18 path, then vswhere)
REM ------------------------------------------------------------------
:find_msbuild
set "MSBUILD_EXE=C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe"
if exist "%MSBUILD_EXE%" goto :eof
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :eof
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -prerelease -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do set "MSBUILD_EXE=%%i"
goto :eof
