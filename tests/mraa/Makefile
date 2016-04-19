include ../../config.mk

ifndef INSTALL_DIR
INSTALL_DIR=/tftp/boot
endif

LIBS   = $(LIB_DEST)/crt0.o -lc
INCS   = -I../../libmraa
CFLAGS = -Wall -Wno-unused-function

PROGS = i2c_write_test i2c_read_test gpio_test ads1115

.PHONY: all clean install

all: $(PROGS)

%: %.c $(LIB_DEST)/libc.a
	$(CC) $(INCS) $(CFLAGS) $< -o $@

clean: 
	rm -f *.o *.d *.map *~ $(PROGS)

install: $(PROGS)
	$(INSTALL_CMD) $(PROGS) $(INSTALL_DIR)
