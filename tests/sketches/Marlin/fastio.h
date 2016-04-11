/*
  This code contibuted by Triffid_Hunter and modified by Kliment
  why double up on these macros? see http://gcc.gnu.org/onlinedocs/cpp/Stringification.html
*/

#include "mraa_gpio.h"

#define NGPIO 26
#define ADC_ADDRESS 0x48

struct gpio_context {
	mraa_gpio_context mraa_cxt;
	char pin_name[10];
	int linux_mapping;
};

extern struct gpio_context gpio_cxt[NGPIO+1];

void WRITE(unsigned IO, int v);
int READ(unsigned IO); 

void SET_INPUT(unsigned IO);
void SET_OUTPUT(unsigned IO);

#define GET_OS_MAPPING(n) (gpio_cxt[n].linux_mapping)

/* vi: set et sw=2 sts=2: */
