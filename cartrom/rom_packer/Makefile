ifeq ($(OS),Windows_NT)
	EXT = .exe
else
	EXT =
endif

TARGET = rom_packer
SRCS = main.c


all: $(TARGET)$(EXT)

$(TARGET)$(EXT): $(SRCS)
	gcc -O2 -o $(TARGET)$(EXT) $(SRCS)

clean:
	rm -f $(TARGET)$(EXT)


.PHONY: all clean
