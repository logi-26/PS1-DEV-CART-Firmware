##### Taken from nolibgs_hello_worlds/thirdparty/nugget/common.mk #####
HAS_LINUX_MIPS_GCC = $(shell which mipsel-linux-gnu-gcc > /dev/null 2> /dev/null && echo true || echo false)

ifeq ($(HAS_LINUX_MIPS_GCC),true)
PREFIX ?= mipsel-linux-gnu
EXT = 
else
PREFIX ?= mipsel-none-elf
EXT = .exe
endif
#######################################################################

TARGET = ps1devcart
TARGET_DEP = rom_rounder
PACKER = rom_packer

AS = $(PREFIX)-as
LD = $(PREFIX)-ld
OBJCOPY = $(PREFIX)-objcopy


all: $(TARGET_DEP)$(EXT) $(PACKER)$(EXT) $(TARGET).rom

# Run make inside $(TARGET_DEP) folder everytime, a bit hacky but works
# https://stackoverflow.com/questions/26226106/how-can-i-force-make-to-execute-a-recipe-all-the-time
$(TARGET_DEP)$(EXT): FORCE
	$(MAKE) -C $(TARGET_DEP)

$(PACKER)$(EXT): FORCE
	$(MAKE) -C $(PACKER)

FORCE: ;

$(TARGET).rom: rom.o
	$(LD) -T ./nugget/rom.ld -Ttext 0x1F000000 --oformat binary -o $(TARGET).rom rom.o
	./rom_rounder/rom_rounder $(TARGET).rom
	cp $(TARGET).rom ../ps1-tools/ar_flash/

# The firmware in the ROM, packed (rom_packer/main.c, unpacked by the entry in rom.s): its PS-X EXE header, then
# exactly the loaded bytes (the ELF's sections, as objcopy writes them: not elf2x's padding to a multiple of 2KB). The
# RAM after them is the .bss, which the firmware's _start() clears.
# Two parts: $(TARGET).pak from 0x400 up to the cheats' area at 0xB000 (PAK_MAX bytes at most), $(TARGET).pak2 after
# the menu images (rom.s).
PAK_MAX = 0xAC00
$(TARGET).pak: ../$(TARGET).ps-exe ../$(TARGET).elf $(PACKER)$(EXT)
	$(OBJCOPY) -O binary ../$(TARGET).elf $(TARGET)_body.tmp
	./$(PACKER)/$(PACKER) ../$(TARGET).ps-exe $(TARGET)_body.tmp $(TARGET).pak $(TARGET).pak2 $(PAK_MAX)
	rm -f $(TARGET)_body.tmp

$(TARGET).pak2: $(TARGET).pak ;

# The menu images (tim.tfs: made by tfs_tool.py, kept unpacked to be edited), each one packed for the ROM
tim_packed.tfs: tim.tfs $(PACKER)$(EXT)
	./$(PACKER)/$(PACKER) -tfs tim.tfs $@

rom.o: $(TARGET).pak $(TARGET).pak2 tim_packed.tfs ../monitor_rom.bin *.inc rom.s
	$(AS) -o rom.o rom.s

clean:
	rm -f *.o $(TARGET).rom $(TARGET).pak $(TARGET).pak2 tim_packed.tfs ../ps1-tools/ar_flash/$(TARGET).rom
	$(MAKE) -C $(TARGET_DEP) clean
	$(MAKE) -C $(PACKER) clean


.PHONY: all clean

