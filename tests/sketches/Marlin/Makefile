include ../../../config.mk
INCS   = -I../../../libarduino/ -I../../../libmraa
CFLAGS = -Wall -DDEBUG #-Werror 
LIBS = -lm -lc

GCODE_TXT=test.gcode
SKETCH_MAIN = ../../../libarduino/main.c
SOURCES = Marlin_main.c planner.c fastio.c stepper.c vector_3.c temperature.c ConfigurationStore.c
OBJ = $(patsubst %.c,%.o,$(SOURCES)) 

all: marlin

marlin: gcode.o $(OBJ)
	$(CC) $(INCS) $(CFLAGS) $^ $(SKETCH_MAIN) -o $@ $(LIBS)

gcode.o: $(GCODE_TXT)
	objcopy --input binary --output elf32-i386 --binary-architecture i386 $^ $@

%.o: %.c
	$(CC) $(INCS) $(CFLAGS) -c $^ 

clean:
	rm -rf *.o marlin

.PHONY: all clean
