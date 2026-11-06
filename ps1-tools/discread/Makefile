include ../../config/Makefile.conf

TARGET = discread

SRCS = $(TARGET).c
CFLAGS = -O2

# Built with PSn00bSDK (see ../../psn00b_tool.mk)
include ../../psn00b_tool.mk


# (the mailbox layout and the drive commands are shared with ps1transfer and the firmware)
$(TARGET).o: discserver.h ../../cdread.h ../../devcart_ui.h

# ps1transfer looks for the disc reader next to its own executable ("-disc" option)
all: ../../ps1transfer/$(TARGET).ps-exe $(SHIPPED_ELF)

../../ps1transfer/$(TARGET).ps-exe: $(TARGET).ps-exe
	cp $(TARGET).ps-exe ../../ps1transfer/$(TARGET).ps-exe

clean:
	rm -f $(OBJS) $(TARGET).elf $(TARGET).ps-exe $(TARGET).map $(TARGET).ld ../../ps1transfer/$(TARGET).ps-exe $(SHIPPED_ELF)


.PHONY: all clean
