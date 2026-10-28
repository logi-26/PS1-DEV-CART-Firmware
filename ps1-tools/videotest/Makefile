include ../../config/Makefile.conf

TARGET = videotest

SRCS = $(TARGET).c ../../psxpcdrv/psxpcdrv.c
CFLAGS = -O2 -I../../psxpcdrv

# Built with PSn00bSDK (see ../../psn00b_tool.mk)
include ../../psn00b_tool.mk


# ps1transfer looks for the video tester next to its own executable (Movie Tools of the PS1 Dev Cart Manager)
all: ../../ps1transfer/$(TARGET).ps-exe $(SHIPPED_ELF)

../../ps1transfer/$(TARGET).ps-exe: $(TARGET).ps-exe
	cp $(TARGET).ps-exe ../../ps1transfer/$(TARGET).ps-exe

clean:
	rm -f $(OBJS) $(TARGET).elf $(TARGET).ps-exe $(TARGET).map $(TARGET).ld ../../ps1transfer/$(TARGET).ps-exe $(SHIPPED_ELF)


.PHONY: all clean
