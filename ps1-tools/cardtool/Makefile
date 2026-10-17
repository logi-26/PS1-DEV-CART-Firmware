include ../../config/Makefile.conf

TARGET = cardtool

# psxpcdrv: PCDRV file server functions (the backup file is written on the computer)
SRCS = $(TARGET).c ../../psxpcdrv/psxpcdrv.c
CFLAGS = -O2 -I../../psxpcdrv

# Built with PSn00bSDK (see ../../psn00b_tool.mk)
include ../../psn00b_tool.mk


# ps1transfer looks for the memory card tool next to its own executable ("-mcbackup"/"-mcrestore" options)
all: ../../ps1transfer/$(TARGET).ps-exe

../../ps1transfer/$(TARGET).ps-exe: $(TARGET).ps-exe
	cp $(TARGET).ps-exe ../../ps1transfer/$(TARGET).ps-exe

clean:
	rm -f $(OBJS) $(TARGET).elf $(TARGET).ps-exe $(TARGET).map $(TARGET).ld ../../ps1transfer/$(TARGET).ps-exe


.PHONY: all clean
