#!/bin/sh
# Puts the Windows build tools in buildtools/ (the layout buildtools/README.txt describes, used by ci/build_*.sh):
#   mingw32/              WinLibs i686 MinGW-w64 GCC 16.2.0 (ps1transfer, the ROM's host tools, the tests' host builds)
#   psn00bsdk/            PSn00bSDK 0.24 with its mipsel-none-elf GCC (firmware, kernel hook, debug monitor)
#   nolibgs_hello_worlds/ the kernel hook's build rules (pinned, with nugget pinned: newer ones break "clean:"), and
#                         psyq/: empty stand-ins for the PsyQ libraries its common.mk links (none is used) and
#                         PSn00bSDK's sys/types.h
#   Makefile.conf         copied to config/Makefile.conf when there is none
# What is already there is kept (CI cache, or a computer set up before). Run from Git Bash: ci/setup_buildtools.sh
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TOOLS="$ROOT/buildtools"
WINLIBS_URL="https://github.com/brechtsanders/winlibs_mingw/releases/download/16.2.0posix-14.0.0-msvcrt-r2/winlibs-i686-posix-dwarf-gcc-16.2.0-mingw-w64msvcrt-14.0.0-r2.zip"
PSN00BSDK_URL="https://github.com/Lameguy64/PSn00bSDK/releases/download/v0.24/PSn00bSDK-0.24-win32.zip"
NOLIBGS_URL="https://github.com/ABelliqueux/nolibgs_hello_worlds"
NOLIBGS_COMMIT=48790325a9e37923dbc8c607aa4b3304316f5451
NUGGET_COMMIT=4fa64028ee0b99c73cab9d7aee125f4d792b53c8
PSYQ_LIBS="api c c2 card cd comb ds etc gpu gs gte gun hmd math mcrd mcx pad press sio snd spu tap"

mkdir -p "$TOOLS"
cd "$TOOLS"

# A zip's folder holding FILE (searched in the extracted zip) moved to DEST
fetch_zip()	# url dest file
{
	echo "Downloading $1"
	rm -rf zip_tmp
	mkdir zip_tmp
	curl -fsSL -o zip_tmp/package.zip "$1"
	python -c "import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])" zip_tmp/package.zip zip_tmp/x
	found="$(find zip_tmp/x -path "*/$3" | head -n 1)"
	if [ -z "$found" ]; then
		echo "$3 not found in $1"
		exit 1
	fi
	dir="$found"
	n=$(echo "$3" | tr -cd '/' | wc -c)
	while [ "$n" -ge 0 ]; do
		dir="$(dirname "$dir")"
		n=$((n - 1))
	done
	mv "$dir" "$2"
	rm -rf zip_tmp
}

if [ ! -x mingw32/bin/gcc.exe ]; then
	fetch_zip "$WINLIBS_URL" mingw32 bin/gcc.exe
fi
if [ ! -x psn00bsdk/bin/mipsel-none-elf-gcc.exe ]; then
	fetch_zip "$PSN00BSDK_URL" psn00bsdk bin/mipsel-none-elf-gcc.exe
fi
if [ ! -f nolibgs_hello_worlds/common.mk ]; then
	echo "Cloning nolibgs_hello_worlds"
	rm -rf nolibgs_hello_worlds
	git clone -q "$NOLIBGS_URL" nolibgs_hello_worlds
	git -C nolibgs_hello_worlds checkout -q "$NOLIBGS_COMMIT"
	git -C nolibgs_hello_worlds submodule update -q --init thirdparty/nugget
	git -C nolibgs_hello_worlds/thirdparty/nugget checkout -q "$NUGGET_COMMIT"
fi
# The PsyQ stand-ins (empty archives) and sys/types.h
mkdir -p nolibgs_hello_worlds/psyq/lib nolibgs_hello_worlds/psyq/include/sys
for l in $PSYQ_LIBS; do
	[ -f "nolibgs_hello_worlds/psyq/lib/lib$l.a" ] || printf '!<arch>\n' > "nolibgs_hello_worlds/psyq/lib/lib$l.a"
done
cp psn00bsdk/include/libpsn00b/sys/types.h nolibgs_hello_worlds/psyq/include/sys/types.h

# config/Makefile.conf pointing here
[ -f Makefile.conf ] || cp "$ROOT/ci/Makefile.conf" Makefile.conf
mkdir -p "$ROOT/config"
[ -f "$ROOT/config/Makefile.conf" ] || cp Makefile.conf "$ROOT/config/Makefile.conf"
echo "Build tools ready in $TOOLS"
