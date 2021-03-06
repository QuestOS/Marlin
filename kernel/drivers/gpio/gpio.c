/*                    The Quest Operating System
 *  Copyright (C) 2005-2012  Richard West, Boston University
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "drivers/gpio/quark_gpio.h"
#include "drivers/gpio/gpio.h"
#include "cy8c9540a.h"
#include "sched/sched.h"
#include "sched/vcpu.h"
#include "util/printf.h"

#define PIN_MODE        0
#define DIG_WRITE       1
#define DIG_READ        2
#define PWM             3
#define INTERRUPT_REG   4
#define INTERRUPT_WAIT  5
#define FAST_DIG_WRITE 			6
#define FAST_DIG_READ  			7

#define OUTPUT  0
#define INPUT   1
#define FAST_OUTPUT 2
#define FAST_INPUT 3
#define HIGH    1
#define LOW     0

//#define DEBUG_GPIO_SYSCALL

#ifdef DEBUG_GPIO_SYSCALL
#define DLOG(fmt,...) DLOG_PREFIX("SYSCALL",fmt,##__VA_ARGS__)
#else
#define DLOG(fmt,...) ;
#endif

static int pwm_enabled[14] = {0};
struct gpio_ops gops;

int
gpio_handler(int operation, int gpio, int val, int extra_arg)
{
	int ret;
  u8 quark_gpio_pin;
  DLOG("op: %u, gpio: %u, val: %u, extra_arg: %u",
      operation, gpio, val, extra_arg);

	switch(operation) {
		case PIN_MODE:
			if (val == OUTPUT) {
#ifdef GALILEO
        ret = gops.set_drive(gpio, GPIOF_DRIVE_STRONG);
        if (ret < 0)
          return ret;
#endif 
				return gops.set_output(gpio, 0);
      } else if (val == INPUT) {
				return gops.set_input(gpio);
      } else {
#ifdef GALILEO
        /* fast mdoe, select the right multiplex line */
        cy8c9540a_fast_gpio_mux(gpio);
        /* set the direction */
        quark_gpio_pin = (gpio == 16) ? 6 : 7;
        int out = (val == FAST_OUTPUT) ? 1 : 0;
        quark_gpio_direction(quark_gpio_pin, out);
#elif MINNOWMAX
        printf("invalid mode!\n");
#endif
        break;
      }
		case DIG_WRITE:
      gops.set_value(gpio, val);
      break;
		case DIG_READ:
			return gops.get_value(gpio);
#ifdef GALILEO
    case FAST_DIG_WRITE:
      quark_gpio_pin = (gpio == 2) ? 6 : 7;
      quark_gpio_write(quark_gpio_pin, val);
      break;
    case FAST_DIG_READ:
      quark_gpio_pin = (gpio == 2) ? 6 : 7;
      return quark_gpio_read(quark_gpio_pin);
    case PWM:
      if (pwm_enabled[gpio] == 0) {
        cy8c9540a_pwm_enable(gpio);
        pwm_enabled[gpio] = 1;
      }
      val = (PERIOD * val) / 255;
      cy8c9540a_pwm_config(gpio, val, PERIOD);
      break;
    case INTERRUPT_REG:
      cy8c9540a_register_interrupt(gpio, val, extra_arg);
      break;
    case INTERRUPT_WAIT:
      cy8c9540a_wait_interrupt(gpio);
      break;
#endif
		default:
			printf("Unsupported operation!");
			return -1;
	}

	return 0;
}

/*
 * Local Variables:
 * indent-tabs-mode: nil
 * mode: C
 * c-file-style: "gnu"
 * c-basic-offset: 2
 * End:
 */

/* vi: set et sw=2 sts=2: */
