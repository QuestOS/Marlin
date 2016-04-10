include ../../../config.mk
INCS   = -I../../../libarduino/
CFLAGS = -Wall -Werror

GCODE_TXT=test.gcode
SKETCH_MAIN = ../../../libarduino/main.c

all: marlin

marlin: gcode.o main.c
	$(CC) $(INCS) $(CFLAGS) $^ $(SKETCH_MAIN) -o $@

gcode.o:
	objcopy --input binary --output elf32-i386 --binary-architecture i386 $(GCODE_TXT) $@

.PHONY: all
