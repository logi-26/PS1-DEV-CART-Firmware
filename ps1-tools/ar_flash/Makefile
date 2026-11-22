include ../../config/Makefile.conf

TARGET = ar_flash

# Built with PSn00bSDK (see ../../psn00b_tool.mk)
SRCS = $(TARGET).c ps1devcart.rom
CFLAGS = -Os

include ../../psn00b_tool.mk


# "ps1transfer -flash" executes the flasher found next to ps1transfer (first, the CD image needs mkpsxiso)
all: ../../ps1transfer/$(TARGET).ps-exe $(TARGET)_nolicense.bin $(TARGET).bin $(TARGET).cue $(SHIPPED_ELF)

../../ps1transfer/$(TARGET).ps-exe: $(TARGET).ps-exe
	cp $(TARGET).ps-exe ../../ps1transfer/$(TARGET).ps-exe

$(TARGET).bin $(TARGET).cue: $(TARGET).ps-exe $(TARGET).xml
	mkpsxiso -y -q $(TARGET).xml

# The same CD image without the Sony licence data (its first 16 sectors empty), committed: the Manager (Firmware page,
# gui/flashercd.py) puts the licence sectors of one of the user's own disc images in it to make the flasher CD
$(TARGET)_nolicense.bin: $(TARGET).ps-exe $(TARGET).xml
	sed -e '/<license /d' -e 's/$(TARGET)\.bin/$(TARGET)_nolicense.bin/' -e 's/$(TARGET)\.cue/$(TARGET)_nolicense.cue/' 		$(TARGET).xml > $(TARGET)_nolicense.xml
	mkpsxiso -y -q $(TARGET)_nolicense.xml
	rm -f $(TARGET)_nolicense.xml $(TARGET)_nolicense.cue

clean:
	rm -f $(OBJS) $(TARGET).elf $(TARGET).ps-exe $(TARGET).map $(TARGET).ld $(TARGET).bin $(TARGET).cue $(TARGET)_nolicense.bin ../../ps1transfer/$(TARGET).ps-exe $(SHIPPED_ELF)


.PHONY: all clean
