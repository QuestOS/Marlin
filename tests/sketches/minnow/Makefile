include ../../../config.mk
INCS   = -I../../../libarduino/ -I../../../libmraa
CFLAGS = -Wall -DDEBUG #-Werror 
LIBS = -lm -lc

SKETCH_MAIN = ../../../libarduino/main.c
SOURCES = minnow.c fastio.c
OBJ = $(patsubst %.c,%.o,$(SOURCES)) 

all: minnow

minnow: $(OBJ)
	$(CC) $(INCS) $(CFLAGS) $^ $(SKETCH_MAIN) -o $@ $(LIBS)

%.o: %.c
	$(CC) $(INCS) $(CFLAGS) -c $^ 

clean:
	rm -rf *.o minnow

.PHONY: all clean
