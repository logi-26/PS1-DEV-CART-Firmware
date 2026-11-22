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
$(TARGET).pak: ../$(TARGET).ps-exe ../$(TARGET).elf $(PACKER)$(EXT)
	$(OBJCOPY) -O binary ../$(TARGET).elf $(TARGET)_body.tmp
	./$(PACKER)/$(PACKER) ../$(TARGET).ps-exe $(TARGET)_body.tmp $@
	rm -f $(TARGET)_body.tmp

rom.o: $(TARGET).pak ../monitor_rom.bin tim.tfs *.inc rom.s
	$(AS) -o rom.o rom.s

clean:
	rm -f *.o $(TARGET).rom $(TARGET).pak ../ps1-tools/ar_flash/$(TARGET).rom
	$(MAKE) -C $(TARGET_DEP) clean
	$(MAKE) -C $(PACKER) clean


.PHONY: all clean

