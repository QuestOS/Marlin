#include <inttypes.h>

#define NGPIO 26
#define ADC_ADDRESS 0x48

#define HIGH 1
#define LOW  0

void WRITE(unsigned IO, int v);
int READ(unsigned IO); 

void SET_INPUT(unsigned IO);
void SET_OUTPUT(unsigned IO);

void minnowmax_gpio_init();
uint16_t ads7828_read_temp();

/* vi: set et sw=2 sts=2: */
