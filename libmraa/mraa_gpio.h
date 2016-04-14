#ifndef __MRAA_GPIO__
#define __MRAA_GPIO__

#include "syscall.h"
#include <stdlib.h>
#include "mraa_types.h"

enum {PIN_DIR, GPIO_WRITE, GPIO_READ};
typedef enum {MRAA_GPIO_OUT, MRAA_GPIO_IN} mraa_gpio_dir_t;

struct _mraa_gpio_context {
	int pin;
};

typedef struct _mraa_gpio_context * mraa_gpio_context;

mraa_gpio_context
mraa_gpio_init(int pin) {
	mraa_gpio_context gc = malloc(sizeof(struct _mraa_gpio_context));
	gc->pin = pin;
	return gc;
}

int
mraa_gpio_write(mraa_gpio_context gc, int value)
{
	return make_gpio_syscall(GPIO_WRITE, gc->pin, value, 0);
}

int
mraa_gpio_read(mraa_gpio_context gc)
{
	return make_gpio_syscall(GPIO_READ, gc->pin, 0, 0);
}

int
mraa_gpio_dir(mraa_gpio_context gc, mraa_gpio_dir_t dir)
{
	return make_gpio_syscall(PIN_DIR, gc->pin, dir, 0);
}

mraa_result_t
mraa_gpio_close(mraa_gpio_context gc)
{
	//TODO: anything to do in the driver??
	if (gc) {
		free(gc);
		return MRAA_SUCCESS;
	}
	return MRAA_ERROR;
}

#endif
