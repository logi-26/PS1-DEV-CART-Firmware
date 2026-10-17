##### Taken from nolibgs_hello_worlds/thirdparty/nugget/common.mk #####
HAS_LINUX_MIPS_GCC = $(shell which mipsel-linux-gnu-gcc > /dev/null 2> /dev/null && echo true || echo false)

ifeq ($(HAS_LINUX_MIPS_GCC),true)
PREFIX ?= mipsel-linux-gnu
else
PREFIX ?= mipsel-none-elf
endif
#######################################################################

# Debug monitor, see monitor.c. Linked with the kernel hook symbols (USB functions, monitor state).
# monitor_rom.bin: in the cartridge flash (cartrom/rom.s), monitor_ram.bin: copied by a firmware test build in RAM.

CC = $(PREFIX)-gcc
OBJCOPY = $(PREFIX)-objcopy

CFLAGS = -Os -Wall -march=mips1 -mabi=32 -EL -msoft-float -fno-pic -mno-shared -mno-abicalls -G0 -mno-gpopt \
	-mlong-calls -fno-builtin -ffreestanding -nostdlib -fno-stack-protector -ffunction-sections -fomit-frame-pointer
LDFLAGS = -nostdlib -static -Wl,--gc-sections -T monitor.ld -Wl,--just-symbols=../kernel_hook/kernel_hook.elf

all: ../monitor_rom.bin ../monitor_ram.bin

monitor_rom.elf: monitor.c monitor.h monitor.ld font8x8.bin ../kernel_hook/kernel_hook.elf
	$(CC) $(CFLAGS) $(LDFLAGS) -Wl,--defsym=MONITOR_ADDR=0x1F01C800 -Wl,-Map=monitor_rom.map -o $@ monitor.c

monitor_ram.elf: monitor.c monitor.h monitor.ld font8x8.bin ../kernel_hook/kernel_hook.elf
	$(CC) $(CFLAGS) $(LDFLAGS) -Wl,--defsym=MONITOR_ADDR=0x801C0000 -Wl,-Map=monitor_ram.map -o $@ monitor.c

../monitor_%.bin: monitor_%.elf
	$(OBJCOPY) -O binary $< $@

clean:
	rm -f *.elf *.map ../monitor_rom.bin ../monitor_ram.bin

.PHONY: all clean
