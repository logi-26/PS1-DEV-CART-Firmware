include ../../config/Makefile.conf

# Audio streamer (see audiostreamer.c): the PlayStation program. The computer runs audio_server.py and the bridge:
#	python audio_server.py
#	ps1transfer -r -e -f audiostreamer.ps-exe -net 127.0.0.1:7777		("make run")
TARGET = audiostreamer

SRCS = $(TARGET).c ../../psxcomb/psxcomb.c
CFLAGS = -O2 -DCOMB_USB_RING=16384

# Built with PSn00bSDK (see ../../psn00b_tool.mk)
include ../../psn00b_tool.mk

run: $(TARGET).ps-exe
	../../ps1transfer/ps1transfer -r -e -f $(TARGET).ps-exe -net 127.0.0.1:7777

clean:
	rm -f $(OBJS) $(TARGET).elf $(TARGET).ps-exe $(TARGET).map $(TARGET).ld

.PHONY: all clean run
