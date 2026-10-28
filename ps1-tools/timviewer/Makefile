include ../../config/Makefile.conf

TARGET = timviewer

# psxpcdrv: PCDRV file server functions (palette editor save/reload)
SRCS = $(TARGET).c ../../psxpcdrv/psxpcdrv.c
CFLAGS = -O2 -I../../psxpcdrv

# Built with PSn00bSDK (see ../../psn00b_tool.mk)
include ../../psn00b_tool.mk


# ps1transfer looks for the TIM viewer next to its own executable ("-tim" option)
all: ../../ps1transfer/$(TARGET).ps-exe $(SHIPPED_ELF)

../../ps1transfer/$(TARGET).ps-exe: $(TARGET).ps-exe
	cp $(TARGET).ps-exe ../../ps1transfer/$(TARGET).ps-exe

clean:
	rm -f $(OBJS) $(TARGET).elf $(TARGET).ps-exe $(TARGET).map $(TARGET).ld ../../ps1transfer/$(TARGET).ps-exe $(SHIPPED_ELF)


.PHONY: all clean
