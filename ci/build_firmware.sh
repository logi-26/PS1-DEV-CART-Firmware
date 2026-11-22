#!/bin/sh
# Builds the firmware (ps1devcart.ps-exe, kernel hook, debug monitor) and the cartridge ROM (cartrom/ps1devcart.rom,
# also copied to ps1-tools/ar_flash) with the tools of buildtools (ci/setup_buildtools.sh). The builds stop with an
# error when a part outgrows its place (the kernel hook's and the monitor's linker scripts, cartrom/rom.s);
# ci/check_sizes.py then reports the room left. Run from Git Bash:
#   ci/build_firmware.sh            build
#   ci/build_firmware.sh clean      clean, then build
# WinLibs' mingw32-make is used, not MSYS make (with that one gcc can't create its temporary files).
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TOOLS="$ROOT/buildtools"
export PATH="$TOOLS/psn00bsdk/bin:$TOOLS/mingw32/bin:$PATH"

[ -f "$ROOT/config/Makefile.conf" ] || cp "$ROOT/ci/Makefile.conf" "$ROOT/config/Makefile.conf"
cd "$ROOT"
if [ "$1" = "clean" ]; then
	mingw32-make SHELL=sh clean
	mingw32-make SHELL=sh -C cartrom clean
fi
mingw32-make SHELL=sh
mingw32-make SHELL=sh -C cartrom
echo "Built cartrom/ps1devcart.rom"
