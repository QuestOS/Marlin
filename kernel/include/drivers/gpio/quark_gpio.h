#ifndef __QUARK_GPIO__
#define __QUARK_GPIO__

#include <types.h>

typedef enum {
	LEVEL = 0,
	EDGE,
} interrupt_type;

typedef enum {
	ACTIVE_LOW = 0,
	ACTIVE_HIGH,
	FALLING_EDGE,
	RISING_EDEG,
} interrupt_polarity;

void quark_gpio_high(u8);
void quark_gpio_low(u8);
void quark_gpio_direction(u8, int);
void quark_gpio_interrupt_enable(u8);
void quark_gpio_interrupt_disable(u8);
void quark_gpio_set_interrupt_type(u8, interrupt_type);
s32 quark_gpio_set_interrupt_polarity(u8, interrupt_polarity);
u32 quark_gpio_read(u8);
void quark_gpio_write(int, int);
void quark_gpio_clear_interrupt(u8 gpio);
void quark_gpio_registers();

#endif
