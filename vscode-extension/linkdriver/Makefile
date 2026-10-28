# The link driver for link play in the emulator (Run Linked, Debug Linked): yario_emu.c (from yaroze-racer), the
# serial port tty a Net Yaroze game has with its boot disc, linked to run in kernel RAM, and the loader the extension
# adds to a copy of the game's psx.exe. Built once with the Net Yaroze toolchain (in WSL); the .bin files come with
# the extension.
#
#   make          linkdriver.bin, loader.bin
#
# DRIVER_ADDR: kernel RAM no BIOS uses (Sony's BIOS: 0x8000C160-0x8000DF7F, used by the PS1 DEV CART firmware's
# kernel hook; OpenBIOS: 0x8000B000-0x8000FFFF found empty while games run). DRIVER_MAX: the room it has.

CC      = mipsel-unknown-ecoff-gcc
OBJCOPY = mipsel-unknown-ecoff-objcopy
NM      = mipsel-unknown-ecoff-nm
CFLAGS  = -O2 -G0 -DYARIO_EMU
DRIVER_ADDR = 8000C400
DRIVER_MAX  = 2048

export PATH := /opt/yaroze/bin:$(PATH)

all: linkdriver.bin loader.bin

# (-G0: the driver runs with the game's $gp, so no gp-relative variables)
%.o: %.c yario_emu.h
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.s
	$(CC) -c $< -o $@

linkdriver.exe: start.o yario_emu.o
	$(CC) -nostartfiles -Xlinker -Ttext -Xlinker $(DRIVER_ADDR) -o $@ start.o yario_emu.o -lps

# The driver's bytes, and its size with its variables (.bss) in linkdriver.size; DRIVER_MAX checked
linkdriver.bin: linkdriver.exe
	$(OBJCOPY) -O binary $< $@
	@end=$$($(NM) $< | sed -n 's/^\([0-9a-fA-F]*\) [A-Za-z] _end$$/\1/p'); \
	size=$$((0x$$end - 0x$(DRIVER_ADDR))); \
	if [ $$size -gt $(DRIVER_MAX) ]; then echo "linkdriver: $$size bytes, more than $(DRIVER_MAX)"; rm -f $@; exit 1; fi; \
	echo "$(DRIVER_ADDR) $$size" > linkdriver.size; echo "linkdriver: $$size bytes at $(DRIVER_ADDR)"

loader.bin: loader.o
	$(OBJCOPY) -O binary -j .text $< $@

clean:
	rm -f *.o linkdriver.exe

.PHONY: all clean
