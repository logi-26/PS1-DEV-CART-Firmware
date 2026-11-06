include ../../config/Makefile.conf

# PS1 LIVE (see live.c): the PlayStation game. Edited while it runs with live_editor.py (editor.bat).
#	ps1transfer -r -e -f live.ps-exe		("make run"), then the editor attaches (ps1transfer -memlive)
TARGET = live

SRCS = $(TARGET).c
CFLAGS = -O2

# Built with PSn00bSDK (see ../../psn00b_tool.mk)
include ../../psn00b_tool.mk

$(OBJS): live.h

# Symbols for debugging (nm output, by address: ps1transfer -sym / -crash, the Manager's Memory page)
all: $(TARGET).sym

$(TARGET).sym: $(TARGET).elf
	$(PSN00BSDK)/bin/mipsel-none-elf-nm -n --defined-only $< | sed "s/^ffffffff//" > $@

run: $(TARGET).ps-exe
	../../ps1transfer/ps1transfer -r -e -f $(TARGET).ps-exe

clean:
	rm -f $(OBJS) $(TARGET).elf $(TARGET).ps-exe $(TARGET).map $(TARGET).ld $(TARGET).sym

.PHONY: all clean run
