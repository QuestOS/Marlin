GCODE_TXT=test.gcode

all: gcode.o

gcode.o:
	objcopy --input binary --output elf32-i386 --binary-architecture i386 $(GCODE_TXT) $@

