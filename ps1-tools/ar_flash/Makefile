include ../../config/Makefile.conf

TARGET = ar_flash

# Built with PSn00bSDK (see ../../psn00b_tool.mk)
SRCS = $(TARGET).c ps1devcart.rom
CFLAGS = -Os

include ../../psn00b_tool.mk


# "ps1transfer -flash" executes the flasher found next to ps1transfer (first, the CD image needs mkpsxiso)
all: ../../ps1transfer/$(TARGET).ps-exe $(TARGET).bin $(TARGET).cue

../../ps1transfer/$(TARGET).ps-exe: $(TARGET).ps-exe
	cp $(TARGET).ps-exe ../../ps1transfer/$(TARGET).ps-exe

$(TARGET).bin $(TARGET).cue: $(TARGET).ps-exe $(TARGET).xml
	mkpsxiso -y -q $(TARGET).xml

clean:
	rm -f $(OBJS) $(TARGET).elf $(TARGET).ps-exe $(TARGET).map $(TARGET).ld $(TARGET).bin $(TARGET).cue ../../ps1transfer/$(TARGET).ps-exe


.PHONY: all clean
