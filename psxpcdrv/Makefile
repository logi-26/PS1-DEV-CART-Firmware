include ../config/Makefile.conf

# psxpcdrv example, built with PSn00bSDK (see ../psn00b_tool.mk):
#	make			file loaded from the computer (psxpcdrv)
#	make CDROM=1	file loaded from the CD-ROM (CD image: make CDROM=1 iso, needs mkpsxiso and config/LICENSEE.DAT)
#	make run		runs it with the file server of ps1transfer
TARGET = devcart_example

SRCS = example.c psxpcdrv.c
CFLAGS = -O2
ifeq ($(CDROM),1)
CFLAGS += -DCDROM_RELEASE
endif

include ../psn00b_tool.mk

iso: $(TARGET).ps-exe
	mkpsxiso -y -q devcart_example.xml

run: $(TARGET).ps-exe
	../ps1transfer/ps1transfer -r -e -f $(TARGET).ps-exe -c -p .

clean:
	rm -f $(OBJS) $(TARGET).elf $(TARGET).ps-exe $(TARGET).map $(TARGET).ld devcart_example.bin devcart_example.cue

.PHONY: all clean iso run
