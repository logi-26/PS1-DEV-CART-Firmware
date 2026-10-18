# PS1 DEV CART with PsyQo: example
# Needs nugget (https://github.com/pcsx-redux/nugget, PsyQo is in it) and a C++20 MIPS toolchain (GCC 10 or later
# with its C++ headers, e.g. the PCSX-Redux one: see nugget's psyqo/GETTING_STARTED.md).
#   make NUGGET=path/to/nugget                    USB link
#   make NUGGET=path/to/nugget LINK_MODE=0        serial link cable
# The cartridge libraries are compiled with the program (plain C, no SDK library).

NUGGET ?= ../../../nugget
LINK_MODE ?= 1
DEVCART = ../..

TARGET = psyqo_example
TYPE = ps-exe

SRCS = psyqo_example.cpp \
$(DEVCART)/psxdevcart/psxdevcart.c \
$(DEVCART)/psxpcdrv/psxpcdrv.c \
$(DEVCART)/psxcomb/psxcomb.c

CPPFLAGS += -I$(DEVCART)/psxdevcart -I$(DEVCART)/psxpcdrv -I$(DEVCART)/psxcomb -I$(NUGGET) -DLINK_MODE=$(LINK_MODE)
CXXFLAGS = -std=c++20

include $(NUGGET)/psyqo/psyqo.mk
