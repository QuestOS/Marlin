#ifndef __GPIO__
#define __GPIO__

#include "syscall.h"

enum {PIN_DIR, GPIO_WRITE, GPIO_READ};
typedef enum {MRAA_GPIO_OUT, MRAA_GPIO_IN} mraa_gpio_dir_t;

typedef struct _mraa_gpio_context {
	int pin;
} mraa_gpio_context;

mraa_gpio_context
mraa_gpio_init(int pin)
{
	mraa_gpio_context gc = {.pin = pin};
	return gc;
}

int
mraa_gpio_write(mraa_gpio_context gc, int value)
{
	return make_gpio_syscall(GPIO_WRITE, gc.pin, value, 0);
}

int
mraa_gpio_read(mraa_gpio_context gc)
{
	return make_gpio_syscall(GPIO_READ, gc.pin, 0, 0);
}

int
mraa_gpio_dir(mraa_gpio_context gc, mraa_gpio_dir_t dir)
{
	return make_gpio_syscall(PIN_DIR, gc.pin, dir, 0);
}

#endif
