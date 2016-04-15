#include "mraa_i2c.h"
#include <unistd.h>
#include <stdio.h>

int main()
{
	mraa_i2c_context ic = mraa_i2c_init(0);
	mraa_i2c_address(ic, 0x8);
	while(1) {
		printf("%d\n", mraa_i2c_read_byte_data(ic, 0));
		usleep(1000000);
	}

	return 0;
}
