/*
  This code contibuted by Triffid_Hunter and modified by Kliment
  why double up on these macros? see http://gcc.gnu.org/onlinedocs/cpp/Stringification.html
*/

#define NGPIO 26
#define ADC_ADDRESS 0x48

#define HIGH 1
#define LOW  0

void WRITE(unsigned IO, int v);
int READ(unsigned IO); 

void SET_INPUT(unsigned IO);
void SET_OUTPUT(unsigned IO);

/* vi: set et sw=2 sts=2: */
