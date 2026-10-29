include ../../config/Makefile.conf

# PS1 CO-PROCESSOR (see coproc.c): the PlayStation program. The computer runs coproc_server.py and the bridge:
#	python coproc_server.py
#	ps1transfer -r -e -f coproc.ps-exe -net 127.0.0.1:7030		("make run", or run.bat)
TARGET = coproc

SRCS = $(TARGET).c ../../psxcomb/psxcomb.c
# The USB link ring is 32 KB (psxcomb's default is 4 KB): a still ray traced frame arrives in one go
CFLAGS = -O2 -DCOMB_USB_RING=32768

# Built with PSn00bSDK (see ../../psn00b_tool.mk)
include ../../psn00b_tool.mk

$(OBJS): coproc.h

run: $(TARGET).ps-exe
	../../ps1transfer/ps1transfer -r -e -f $(TARGET).ps-exe -net 127.0.0.1:7030

clean:
	rm -f $(OBJS) $(TARGET).elf $(TARGET).ps-exe $(TARGET).map $(TARGET).ld

.PHONY: all clean run
