include ../../config/Makefile.conf

# PS1 LIVE (see live.c): the PlayStation game. Edited while it runs with live_editor.py (editor.bat).
#	ps1transfer -r -e -f live.ps-exe		("make run"), then the editor attaches (ps1transfer -memlive)
TARGET = live

SRCS = $(TARGET).c
CFLAGS = -O2

# Built with PSn00bSDK (see ../../psn00b_tool.mk)
include ../../psn00b_tool.mk

$(OBJS): live.h

run: $(TARGET).ps-exe
	../../ps1transfer/ps1transfer -r -e -f $(TARGET).ps-exe

clean:
	rm -f $(OBJS) $(TARGET).elf $(TARGET).ps-exe $(TARGET).map $(TARGET).ld

.PHONY: all clean run
