include ../config.mk

ifndef INSTALL_DIR
INSTALL_DIR=/tftp/boot
endif

LIBS   = -lm -lc $(LIB_DEST)/crt0.o
INCS   = 
CFLAGS =  -g -Wall -O0 -MMD $(INCS)

OBJS = torcs.o

all:  torcs

torcs: $(OBJS) 
	$(LD) -o torcs $(OBJS) $(LIBS)


%.o: %.c
	$(CC)  $(CFLAGS) -o $@ -c $<

clean:
	rm -f *.o *.d *~ torcs

install: torcs
	$(INSTALL_CMD) torcs $(INSTALL_DIR)
