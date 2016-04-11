include ../../../config.mk
INCS   = -I../../../libarduino/
CFLAGS = -Wall #-Werror 
LIBS = -lm

GCODE_TXT=test.gcode
SKETCH_MAIN = ../../../libarduino/main.c

all: marlin

marlin: gcode.o main.c planner.c
	$(CC) $(INCS) $(CFLAGS) $^ $(SKETCH_MAIN) -o $@ $(LIBS)

gcode.o:
	objcopy --input binary --output elf32-i386 --binary-architecture i386 $(GCODE_TXT) $@

clean:
	rm -rf *.o marlin

.PHONY: all clean
