include ../../config/Makefile.conf

TARGET = timingtest

SRCS = $(TARGET).c
CFLAGS = -O2

# Built with PSn00bSDK (see ../../psn00b_tool.mk)
include ../../psn00b_tool.mk


# (the mailbox layout and the test pattern are shared with ps1transfer)
$(TARGET).o: timingtest.h ../../devcart_ui.h

# ps1transfer looks for the test program next to its own executable ("-timingtest" option)
all: ../../ps1transfer/$(TARGET).ps-exe

../../ps1transfer/$(TARGET).ps-exe: $(TARGET).ps-exe
	cp $(TARGET).ps-exe ../../ps1transfer/$(TARGET).ps-exe

clean:
	rm -f $(OBJS) $(TARGET).elf $(TARGET).ps-exe $(TARGET).map $(TARGET).ld ../../ps1transfer/$(TARGET).ps-exe


.PHONY: all clean
