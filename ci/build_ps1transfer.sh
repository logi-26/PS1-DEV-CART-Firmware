#!/bin/sh
# Builds ps1transfer/ps1transfer.exe (Windows, 32-bit) with the MinGW of buildtools (ci/setup_buildtools.sh): every
# .c file of ps1transfer/ (ci/check_build_lists.py checks that its Makefiles and projects list them all).
# Run from Git Bash: ci/build_ps1transfer.sh
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TOOLS="$ROOT/buildtools"
OBJ="$TOOLS/obj/ps1transfer"
export PATH="$TOOLS/mingw32/bin:$PATH"

mkdir -p "$OBJ"
cd "$ROOT/ps1transfer"
objs=""
for src in *.c; do
	f="${src%.c}"
	echo "gcc $src"
	gcc -Wall -m32 -O2 -c "$src" -o "$OBJ/$f.o"
	objs="$objs $OBJ/$f.o"
done
g++ -L. -o "$OBJ/ps1transfer.exe" $objs -m32 -s -static-libgcc -static-libstdc++ -lftd2xx -lws2_32 -lsetupapi \
	-lcfgmgr32
cp "$OBJ/ps1transfer.exe" ps1transfer.exe
echo "Built ps1transfer/ps1transfer.exe"
