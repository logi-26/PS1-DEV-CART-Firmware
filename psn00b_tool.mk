# Build rules for the PS1 tools (ps1-tools/...) and the psxpcdrv example with PSn00bSDK
# (open source, https://github.com/Lameguy64/PSn00bSDK, set PSN00BSDK in config/Makefile.conf).
# The tool Makefile sets TARGET, SRCS (.c files and binary files turned into objects) and optionally CFLAGS, LOAD_ADDR,
# then includes this file.

# Repository root (this file's folder, e.g. ../../ for a tool in ps1-tools): shared headers (devcart_ui.h, ...)
DEVCART_ROOT := $(dir $(lastword $(MAKEFILE_LIST)))

PSN00B_LIB = $(PSN00BSDK)/lib/libpsn00b
PSN00B_CC = $(PSN00BSDK)/bin/mipsel-none-elf-gcc
PSN00B_OBJCOPY = $(PSN00BSDK)/bin/mipsel-none-elf-objcopy
ELF2X = $(PSN00BSDK)/bin/elf2x

LOAD_ADDR ?= 0x80010000
# Stack at the top of RAM in the PS-EXE header (elf2x leaves 0: the BIOS would keep the caller's stack)
STACK_ADDR_BYTES ?= '\000\377\037\200'

# Same flags as PSn00bSDK's CMake setup (lib/libpsn00b/cmake/flags.cmake), executable without GP-relative data
PSN00B_CFLAGS = -g -G0 -ffreestanding -fno-builtin -nostdlib -fdata-sections -ffunction-sections -fsigned-char \
	-fno-strict-overflow -msoft-float -march=r3000 -mtune=r3000 -mabi=32 -mno-mt -mno-llsc -fno-pic -mno-abicalls \
	-mno-gpopt -DPSN00BSDK=1 -I$(PSN00BSDK)/include/libpsn00b -I$(DEVCART_ROOT)
PSN00B_LIBS = -L$(PSN00B_LIB)/release -lpsxcd_exe_nogprel -lpsxspu_exe_nogprel -lpsxgte_exe_nogprel -lpsxgpu_exe_nogprel \
	-lpsxpress_exe_nogprel -lpsxetc_exe_nogprel -lpsxapi_exe_nogprel -lc_exe_nogprel

OBJS = $(addsuffix .o, $(basename $(SRCS)))

all: $(TARGET).ps-exe

%.o: %.c
	$(PSN00B_CC) $(PSN00B_CFLAGS) $(CFLAGS) -c $< -o $@

# Binary file -> object with _binary_<name>_<ext>_start/_end/_size symbols (as PsyQ/nugget's OBJCOPYME)
%.o: %.tim
	$(PSN00B_OBJCOPY) -I binary --set-section-alignment .data=4 --rename-section .data=.rodata,alloc,load,readonly,data,contents \
		-O elf32-littlemips -B mips $< $@
%.o: %.rom
	$(PSN00B_OBJCOPY) -I binary --set-section-alignment .data=4 --rename-section .data=.rodata,alloc,load,readonly,data,contents \
		-O elf32-littlemips -B mips $< $@

# PSn00bSDK linker script with the program loaded at LOAD_ADDR
$(TARGET).ld: $(PSN00B_LIB)/ldscripts/exe.ld
	sed -e 's/ORIGIN = 0x80010000, LENGTH = 0x7f0000/ORIGIN = $(LOAD_ADDR), LENGTH = 0x80200000 - $(LOAD_ADDR)/' $< > $@

$(TARGET).elf: $(OBJS) $(TARGET).ld
	$(PSN00B_CC) -nostdlib -static -G0 -Wl,-gc-sections -T $(TARGET).ld -Wl,-Map=$(TARGET).map -o $@ $(OBJS) \
		-Wl,--start-group $(PSN00B_LIBS) -Wl,--end-group

$(TARGET).ps-exe: $(TARGET).elf
	$(ELF2X) -q $< $@
	printf $(STACK_ADDR_BYTES) | dd of=$@ bs=1 seek=48 conv=notrunc 2> /dev/null
