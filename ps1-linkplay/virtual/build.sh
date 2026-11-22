#!/bin/sh
# Builds the virtual link driver:
#   vlink.bin     emulator (vlink.lua): code at 0x8000C000, rings at 0x8000D000 (free RAM under OpenBIOS), 4 KB rings
# (the PS1 DEV CART build: build_hw.py)
set -e
cd "$(dirname "$0")"
export PATH="$PATH:${MIPS_TOOLS:-$(cygpath -u "$APPDATA")/mips/mips/bin}"
CC=mipsel-none-elf-gcc
FLAGS="-Os -G0 -march=r3000 -msoft-float -mno-abicalls -fno-pic -ffreestanding -fno-builtin -nostdlib -mno-gpopt"
build() {	# name code buf extra-flags
	$CC $FLAGS -DVLINK_BUF=$3 $4 -Wl,-Ttext=$2 -Wl,--section-start=.text.head=$2 -Wl,-e,0 		-o $1.elf vlink_head.S vlink.c
	mipsel-none-elf-objcopy -O binary -j .text.head -j .text -j .rodata -j .data $1.elf $1.bin
	if mipsel-none-elf-objdump -h $1.elf | grep -q -E ' \.(s?bss|s?data) '; then echo "$1: has data/bss"; exit 1; fi
	echo "$1.bin: $(wc -c < $1.bin) bytes at $2, rings at $3"
}
build vlink 0x8000C000 0x8000D000 ""
