#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include "ardutime.h"
#include "fastio.h"
#include "arduthread.h"

void setup() {
	/* this can be easily merged into main()
	 * so that it will be hidden from user. */
  minnowmax_gpio_init();
	/* ************************* */

	pinMode(5, OUTPUT);
	pinMode(6, OUTPUT);
}

void loop(1, 30, 100) {
	delay(2000);
  digitalWrite(5, 1);
	delay(2000);
  digitalWrite(5, 0);
}

void loop(2, 30, 100) {
	delay(3000);
  digitalWrite(6, 1);
	delay(3000);
  digitalWrite(6, 0);
}
