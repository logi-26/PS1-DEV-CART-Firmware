include ../../config/Makefile.conf

# PS1 REWIND (see rewind.h): PEG DROP, a game using the rewind library. The computer runs rewind_server.py and the
# bridge:
#	python rewind_server.py
#	ps1transfer -r -e -f pegdrop.ps-exe -net 127.0.0.1:7040		("make run", or run.bat)
TARGET = pegdrop

SRCS = $(TARGET).c rewind.c ../../psxcomb/psxcomb.c
# The USB link ring is 32 KB (psxcomb's default is 4 KB): older frames come in bursts while rewinding
CFLAGS = -O2 -DCOMB_USB_RING=32768

# Built with PSn00bSDK (see ../../psn00b_tool.mk)
include ../../psn00b_tool.mk

$(OBJS): rewind.h

run: $(TARGET).ps-exe
	../../ps1transfer/ps1transfer -r -e -f $(TARGET).ps-exe -net 127.0.0.1:7040

clean:
	rm -f $(OBJS) $(TARGET).elf $(TARGET).ps-exe $(TARGET).map $(TARGET).ld

.PHONY: all clean run
