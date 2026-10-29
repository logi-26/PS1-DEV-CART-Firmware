include ../../config/Makefile.conf

# PS1 SLI (see sli.c): one 3D scene drawn by two PlayStations, half each, in lockstep over the link cable.
#	run.bat (both consoles), or on each: ps1transfer -n SERIAL -link -f sli.ps-exe
TARGET = sli

SRCS = $(TARGET).c ../../psxcomb/psxcomb.c
CFLAGS = -O2

# Built with PSn00bSDK (see ../../psn00b_tool.mk)
include ../../psn00b_tool.mk

clean:
	rm -f $(OBJS) $(TARGET).elf $(TARGET).ps-exe $(TARGET).map $(TARGET).ld

.PHONY: all clean
