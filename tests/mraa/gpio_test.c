#include "mraa_gpio.h"
#include <unistd.h>
#include <stdio.h>

int main()
{
	mraa_gpio_context gc = mraa_gpio_init(10);
	mraa_gpio_context gc1 = mraa_gpio_init(11);
	mraa_gpio_dir(gc, MRAA_GPIO_OUT);
	mraa_gpio_dir(gc1, MRAA_GPIO_IN);
	while(1) {
		printf("%d\n", mraa_gpio_read(gc1));
		mraa_gpio_write(gc, 1);
		usleep(1000000);
		mraa_gpio_write(gc, 0);
		usleep(1000000);
	}

	return 0;
}
