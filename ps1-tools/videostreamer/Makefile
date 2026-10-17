include ../../config/Makefile.conf

# PS1 DEV CART video streamer (see videostreamer.c): the PlayStation program. The computer runs video_server.py and
# the bridge:
#	python video_server.py [video files or URLs]
#	ps1transfer -r -e -f videostreamer.ps-exe -net 127.0.0.1:7778		("make run")
TARGET = videostreamer

SRCS = $(TARGET).c ../../psxcomb/psxcomb.c
CFLAGS = -O2 -DCOMB_USB_RING=16384

# Built with PSn00bSDK (see ../../psn00b_tool.mk)
include ../../psn00b_tool.mk

run: $(TARGET).ps-exe
	../../ps1transfer/ps1transfer -r -e -f $(TARGET).ps-exe -net 127.0.0.1:7778

clean:
	rm -f $(OBJS) $(TARGET).elf $(TARGET).ps-exe $(TARGET).map $(TARGET).ld

.PHONY: all clean run
