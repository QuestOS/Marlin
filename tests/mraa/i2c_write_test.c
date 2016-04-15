#include "mraa_i2c.h"
#include <unistd.h>

int main()
{
	mraa_i2c_context ic = mraa_i2c_init(0);
	mraa_i2c_address(ic, 0x8);
	while(1) {
		//mraa_i2c_write_byte(ic, 'q');
		mraa_i2c_write_word(ic, 300);
		usleep(1000000);
	}

	return 0;
}
