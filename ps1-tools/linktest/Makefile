include ../../config/Makefile.conf

TARGET = linktest

SRCS = $(TARGET).c ../../psxcomb/psxcomb.c
CFLAGS = -O2

# Built with PSn00bSDK (see ../../psn00b_tool.mk)
include ../../psn00b_tool.mk


# ps1transfer looks for the link cable tester next to its own executable ("-link" option)
all: ../../ps1transfer/$(TARGET).ps-exe

../../ps1transfer/$(TARGET).ps-exe: $(TARGET).ps-exe
	cp $(TARGET).ps-exe ../../ps1transfer/$(TARGET).ps-exe

# PsyQ build with the libcomb mode (linktest_psyq.ps-exe), optional: needs PSXDIR (PsyQ)
psyq:
	$(MAKE) -f Makefile.psyq

clean:
	rm -f $(OBJS) $(TARGET).elf $(TARGET).ps-exe $(TARGET).map $(TARGET).ld ../../ps1transfer/$(TARGET).ps-exe


.PHONY: all clean psyq
