#!/bin/bash
# Build OpenTyrian 2000 for the Sony PS Vita -> vita/build/OpenTyrian2000.vpk
#
#   bash vita/build.sh          # build
#   bash vita/build.sh clean    # remove the build dir
#
# On this Windows machine the VitaSDK build is driven by a NATIVE Windows cmake + ninja, which
# must be handed native (D:\...) paths. MSYS bash mangles those paths (and splits the colon in
# the VITASDK env var), so the real build logic lives in the companion PowerShell script,
# vita/build.ps1. This wrapper just forwards to it so the familiar `bash vita/build.sh` works
# too -- matching the Switch build.sh convention. See vita/README.md for details and for the
# native cmake/ninja requirements.
set -e
cd "$(dirname "$0")"

PS_ARGS=""
[ "$1" = "clean" ] && PS_ARGS="-Clean"

# Absolute Windows path to build.ps1 so PowerShell finds it regardless of how bash was launched.
PS1_WIN=$(cygpath -w "$PWD/build.ps1" 2>/dev/null || echo "build.ps1")

exec powershell.exe -ExecutionPolicy Bypass -File "$PS1_WIN" $PS_ARGS
