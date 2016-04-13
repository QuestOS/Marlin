include ../../../config.mk
INCS   = -I../../../libarduino/ -I../../../libmraa
CFLAGS = -Wall #-Werror 
LIBS = -lm -lc

GCODE_TXT=test.gcode
SKETCH_MAIN = ../../../libarduino/main.c

all: marlin

marlin: gcode.o main.c planner.c fastio.c stepper.c 
	$(CC) $(INCS) $(CFLAGS) $^ $(SKETCH_MAIN) -o $@ $(LIBS)

gcode.o:
	objcopy --input binary --output elf32-i386 --binary-architecture i386 $(GCODE_TXT) $@

clean:
	rm -rf *.o marlin

.PHONY: all clean
