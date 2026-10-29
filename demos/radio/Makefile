include ../../config/Makefile.conf

# PS1 DEV CART Radio (see radio.c): the PlayStation program. The computer runs radio_server.py and the bridge:
#	python radio_server.py
#	ps1transfer -r -e -f radio.ps-exe -net 127.0.0.1:7777		("make run", or run.bat)
TARGET = radio

SRCS = $(TARGET).c ../../psxcomb/psxcomb.c
# The USB link ring is 16 KB (psxcomb's default is 4 KB): the audio arrives in bursts
CFLAGS = -O2 -DCOMB_USB_RING=16384

# Built with PSn00bSDK (see ../../psn00b_tool.mk)
include ../../psn00b_tool.mk

run: $(TARGET).ps-exe
	../../ps1transfer/ps1transfer -r -e -f $(TARGET).ps-exe -net 127.0.0.1:7777

clean:
	rm -f $(OBJS) $(TARGET).elf $(TARGET).ps-exe $(TARGET).map $(TARGET).ld

.PHONY: all clean run
