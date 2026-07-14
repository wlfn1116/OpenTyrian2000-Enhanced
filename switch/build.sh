#!/bin/bash
# Build OpenTyrian 2000 for Nintendo Switch (.nro).
#
# Invoke with the devkitPro MSYS2 bash, e.g. from PowerShell:
#   & "D:\devkitPro\msys2\usr\bin\bash.exe" /d/Projects/OpenTyrian2000-widescreen/switch/build.sh
# or from the devkitPro shell:
#   bash switch/build.sh          # build   (default target)
#   bash switch/build.sh clean    # clean
#
# A directly-invoked (non-login) shell starts with an almost-empty PATH, so we set
# it explicitly before touching any coreutil. Output is teed to switch/build.log.

export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
export DEVKITA64=$DEVKITPRO/devkitA64
export PATH="/usr/bin:$DEVKITPRO/tools/bin:$DEVKITA64/bin:$DEVKITPRO/portlibs/switch/bin:$PATH"

cd "$(dirname "$0")" || exit 1

make "$@" 2>&1 | tee build.log
exit ${PIPESTATUS[0]}
