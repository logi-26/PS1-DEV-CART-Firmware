include config/Makefile.conf

# The firmware is built with PSn00bSDK (open source, https://github.com/Lameguy64/PSn00bSDK, set
# PSN00BSDK in config/Makefile.conf) instead of PsyQ. The kernel hook and the debug monitor use no SDK library
# (kernel_hook: nugget build rules from PSXDIR, monitor: plain gcc).

TARGET = ps1devcart
TARGET_DEP = kernel_hook

PSN00B_LIB = $(PSN00BSDK)/lib/libpsn00b
CC = $(PSN00BSDK)/bin/mipsel-none-elf-gcc
OBJCOPY = $(PSN00BSDK)/bin/mipsel-none-elf-objcopy
ELF2X = $(PSN00BSDK)/bin/elf2x

# Same flags as PSn00bSDK's CMake setup (lib/libpsn00b/cmake/flags.cmake), executable without GP-relative data
CFLAGS = -O2 -g -G0 -ffreestanding -fno-builtin -nostdlib -fdata-sections -ffunction-sections -fsigned-char \
	-fno-strict-overflow -msoft-float -march=r3000 -mtune=r3000 -mabi=32 -mno-mt -mno-llsc -fno-pic -mno-abicalls \
	-mno-gpopt -DPSN00BSDK=1 -I$(PSN00BSDK)/include/libpsn00b
LIBS = -L$(PSN00B_LIB)/release -lpsxcd_exe_nogprel -lpsxspu_exe_nogprel -lpsxgpu_exe_nogprel -lpsxetc_exe_nogprel \
	-lpsxapi_exe_nogprel -lc_exe_nogprel
OBJS = $(TARGET).o $(TARGET_DEP).o

ifeq ($(RAMTEST),1)
# Test in RAM ("make RAMTEST=1", executed with ps1transfer): the debug monitor is copied to RAM
TLOAD_ADDR = 0x801D0000
CFLAGS += -DMONITOR_IN_RAM -DTFS_RAM_ADRS=0x80140000
OBJS += monitor_ram.o
else
# Release in ROM (the debug monitor is in the cartridge flash, see cartrom/rom.s)
TLOAD_ADDR = 0x801EF000
endif
# The profiler's RAM fill (Execute) stops under the firmware
CFLAGS += -DFW_LOAD_ADDR=$(TLOAD_ADDR)

all: $(TARGET).ps-exe monitor_rom.bin

$(TARGET).o: $(TARGET).c display.h usb.h tfs.h psyq_compat.h cdread.h monitor/monitor.h FORCE
	$(CC) $(CFLAGS) -c $< -o $@

# PSn00bSDK linker script with the program loaded at TLOAD_ADDR (top of RAM, under the stack)
$(TARGET).ld: $(PSN00B_LIB)/ldscripts/exe.ld FORCE
	sed -e 's/ORIGIN = 0x80010000, LENGTH = 0x7f0000/ORIGIN = $(TLOAD_ADDR), LENGTH = 0x80200000 - $(TLOAD_ADDR)/' $< > $@

$(TARGET).elf: $(OBJS) $(TARGET).ld
	$(CC) -nostdlib -static -G0 -Wl,-gc-sections -T $(TARGET).ld -Wl,-Map=$(TARGET).map -o $@ $(OBJS) \
		-Wl,--start-group $(LIBS) -Wl,--end-group

$(TARGET).ps-exe: $(TARGET).elf
	$(ELF2X) -q $< $@
	# Stack at the top of RAM in the header (s_addr = 0x801FFF00, like the PsyQ build; elf2x leaves 0)
	printf '\000\377\037\200' | dd of=$@ bs=1 seek=48 conv=notrunc 2> /dev/null

# Run make inside $(TARGET_DEP) folder everytime, a bit hacky but works
# https://stackoverflow.com/questions/26226106/how-can-i-force-make-to-execute-a-recipe-all-the-time
$(TARGET_DEP).bin: FORCE
	$(MAKE) -C $(TARGET_DEP)

# Debug monitor: linked with the kernel hook symbols, so built after it
monitor_ram.bin monitor_rom.bin: $(TARGET_DEP).bin FORCE
	$(MAKE) -C monitor

# Binary file -> object with _binary_<name>_start/_size symbols
%.o: %.bin
	$(OBJCOPY) -I binary --set-section-alignment .data=4 --rename-section .data=.rodata,alloc,load,readonly,data,contents \
		-O elf32-littlemips -B mips $< $@

FORCE: ;

clean:
	rm -f *.o $(TARGET).elf $(TARGET).ps-exe $(TARGET).map $(TARGET).ld
	$(MAKE) -C $(TARGET_DEP) clean
	$(MAKE) -C monitor clean

.PHONY: all clean
