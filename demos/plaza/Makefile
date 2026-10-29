include ../../config/Makefile.conf

# PS1 PLAZA (see plaza.c): the PlayStation program of an online game. The computer bridges it to the game server:
#	ps1transfer -r -e -f plaza.ps-exe -net play.logi26.co.uk:7020		("make run", or run.bat)
TARGET = plaza
SERVER ?= play.logi26.co.uk:7020

SRCS = $(TARGET).c ../../psxcomb/psxcomb.c
# The USB link ring is 16 KB (psxcomb's default is 4 KB): the board arrives in one go when you join
CFLAGS = -O2 -DCOMB_USB_RING=16384

# Built with PSn00bSDK (see ../../psn00b_tool.mk)
include ../../psn00b_tool.mk

$(OBJS): plaza.h

run: $(TARGET).ps-exe
	../../ps1transfer/ps1transfer -r -e -f $(TARGET).ps-exe -net $(SERVER)

clean:
	rm -f $(OBJS) $(TARGET).elf $(TARGET).ps-exe $(TARGET).map $(TARGET).ld

.PHONY: all clean run
