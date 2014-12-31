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

#include "drivers/pci/pci.h"
#include "drivers/i2c/galileo_i2c.h"
#include "util/printf.h"
#include "mem/mem.h"

#define DEBUG_QGPIO

#ifdef DEBUG_QGPIO
#define DLOG(fmt,...) DLOG_PREFIX("Quark GPIO",fmt,##__VA_ARGS__)
#else
#define DLOG(fmt,...) ;
#endif

#define GPIO_SWPORTA_DR 		0x00
#define GPIO_SWPORTA_DDR		0x04
#define GPIO_INTEN					0x30
#define GPIO_INTMASK				0x34
#define GPIO_INTTYPE_LEVEL 	0x38
#define GPIO_INT_POLARITY		0x3C
#define GPIO_INTSTATUS			0x40
#define GPIO_RAW_INTSTATUS	0x44
#define GPIO_DEBOUNCE				0x48
#define GPIO_PORTA_EOI			0x4C
#define GPIO_EXT_PORTA			0x50
#define GPIO_LS_SYNC				0x60

static void *mmio_base;

static inline void
qgpio_write_r(u32 val , u32 reg)
{
  *(u32 *)((u32)mmio_base + reg) = val;
}

static inline u32
qgpio_read_r(u32 reg)
{
  return *(u32 *)((u32)mmio_base + reg);
}

void 
quark_gpio_high(u8 gpio)
{
	qgpio_write_r((1 << gpio) | qgpio_read_r(GPIO_SWPORTA_DR), GPIO_SWPORTA_DR);
}

void 
quark_gpio_low(u8 gpio)
{
	qgpio_write_r(~(1 << gpio) & qgpio_read_r(GPIO_SWPORTA_DR), GPIO_SWPORTA_DR);
}

void
quark_gpio_direction(u8 gpio, int out)
{
	u32 val = qgpio_read_r(GPIO_SWPORTA_DDR);
	if (out) 
		val |= (1 << gpio);
	else
		val &= ~(1 << gpio);
	qgpio_write_r(val, GPIO_SWPORTA_DDR);
}

void 
quark_gpio_interrupt_enable(u8 gpio)
{
	qgpio_write_r((1 << gpio) | qgpio_read_r(GPIO_INTEN), GPIO_INTEN);
	DLOG("quark gpio interrupt enable reg is 0x%x", qgpio_read_r(GPIO_INTEN)); 
}

void 
quark_gpio_interrupt_disable(u8 gpio)
{
	qgpio_write_r(~(1 << gpio) & qgpio_read_r(GPIO_INTEN), GPIO_INTEN);
}

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

void 
quark_gpio_set_interrupt_type(u8 gpio, interrupt_type type)
{
	if (type == EDGE)
		qgpio_write_r((1 << gpio) | qgpio_read_r(GPIO_INTTYPE_LEVEL), GPIO_INTTYPE_LEVEL);
	else
		qgpio_write_r(~(1 << gpio) & qgpio_read_r(GPIO_INTTYPE_LEVEL), GPIO_INTTYPE_LEVEL);
}

s32 
quark_gpio_set_interrupt_polarity(u8 gpio, interrupt_polarity polarity)
{
	interrupt_type type = qgpio_read_r(GPIO_INTTYPE_LEVEL);

	switch (polarity) {
		case ACTIVE_LOW:
		case ACTIVE_HIGH:
			if (type != LEVEL)
				return -1;
		case FALLING_EDGE:
		case RISING_EDEG:
			if (type != EDGE)
				return -1;
	}
	switch (polarity) {
		case ACTIVE_LOW:
		case FALLING_EDGE:
			qgpio_write_r(~(1 << gpio) & qgpio_read_r(GPIO_INT_POLARITY), GPIO_INT_POLARITY);
			return 0;
		case ACTIVE_HIGH:
		case RISING_EDEG:
			qgpio_write_r((1 << gpio) | qgpio_read_r(GPIO_INT_POLARITY), GPIO_INT_POLARITY);
			return 0;
	}
}

u8
quark_gpio_read_port_status()
{
	return (u8)qgpio_read_r(GPIO_EXT_PORTA);
}

void
quark_gpio_registers()
{
	DLOG("int mask 0x%x", qgpio_read_r(GPIO_INTMASK));
	DLOG("int status 0x%x", qgpio_read_r(GPIO_INTSTATUS));
	DLOG("int level 0x%x", qgpio_read_r(GPIO_INTTYPE_LEVEL));
	DLOG("int polarity 0x%x", qgpio_read_r(GPIO_INT_POLARITY));
	DLOG("int enable 0x%x", qgpio_read_r(GPIO_INTEN));
	DLOG("int direction 0x%x", qgpio_read_r(GPIO_SWPORTA_DDR));
	DLOG("int data 0x%x", qgpio_read_r(GPIO_SWPORTA_DR));
}

void
quark_gpio_clear_interrupt(u8 gpio)
{
	qgpio_write_r((1 << gpio), GPIO_PORTA_EOI);
}

#define CYPRESS_INT_LINE 5

static uint32
quark_irq_handler(uint8 vec)
{
	quark_gpio_clear_interrupt(CYPRESS_INT_LINE);
	cy8c9540a_irq_handler();
	return 0;
}

static uint32
shared_irq_handler(uint8 vec)
{
	sint32 ret;
	if ((ret = i2c_irq_handler(vec)) < 0)
		/* this interrupt is not for i2c
		 * so it must be for quark gpio */
		return quark_irq_handler(vec);
	return ret;
}

#define GALILEO_QGPIO_VID			0x8086
#define	GALILEO_QGPIO_DID		 	0x0934

static pci_device quark_gpio_pci_device;

bool
quark_gpio_init()
{
	uint device_index, irq_line, irq_pin;
	uint mem_addr;
	pci_irq_t irq;

	if (!pci_find_device(GALILEO_QGPIO_VID, GALILEO_QGPIO_DID,
				0xFF, 0xFF, 0, &device_index))
		return FALSE;
	if (device_index == (uint)(~0)) {
    DLOG ("Unable to detect compatible device.");
    return FALSE;
  }
	DLOG ("Found device_index=%d", device_index);

	if (!pci_get_device(device_index, &quark_gpio_pci_device)) {
		DLOG("Unable to get PCI device from PCI subsystem");
		return FALSE;
	}
  DLOG ("Using PCI bus=%x dev=%x func=%x",
        quark_gpio_pci_device.bus,
        quark_gpio_pci_device.slot,
        quark_gpio_pci_device.func);

	if (!pci_decode_bar (device_index, 1, &mem_addr, NULL, NULL)) {
    DLOG ("Invalid PCI configuration or BAR0 not found");
    return FALSE;
  } 
	if (mem_addr == 0) {
    DLOG ("Unable to detect memory mapped IO region");
    return FALSE;
  }
  mmio_base = map_virtual_page (mem_addr | 0x3);
  if (mmio_base == NULL) {
    DLOG ("Unable to map page to phys=%p", mem_addr);
    return FALSE;
  }
  DLOG ("Using memory mapped IO at phys=%p virt=%p", mem_addr, mmio_base);

	if (!pci_get_interrupt(device_index, &irq_line, &irq_pin)) {
		DLOG("Unable to get IRQ");
		goto abort;
	}
	if (pci_irq_find(quark_gpio_pci_device.bus, quark_gpio_pci_device.slot,
				irq_pin, &irq)) {
		/* use PCI routing table */
    DLOG ("Found PCI routing entry irq.gsi=0x%x", irq.gsi);
		if (!pci_irq_map_handler(&irq, shared_irq_handler, get_logical_dest_addr(0),
					IOAPIC_DESTINATION_LOGICAL,
					IOAPIC_DELIVERY_FIXED))
			goto abort;
		irq_line = irq.gsi;
	}
  DLOG ("Using IRQ line=%.02X pin=%X", irq_line, irq_pin);

	/* set up cypress interrupt */
	quark_gpio_set_interrupt_type(CYPRESS_INT_LINE, EDGE);
	quark_gpio_set_interrupt_polarity(CYPRESS_INT_LINE, RISING_EDEG);
	quark_gpio_clear_interrupt(CYPRESS_INT_LINE);
	quark_gpio_interrupt_enable(CYPRESS_INT_LINE);
	return TRUE;

abort:
	unmap_virtual_page(mmio_base);
	return FALSE;
}

#include "module/header.h"

static const struct module_ops mod_ops = {
  .init = quark_gpio_init
};

DEF_MODULE (galileo_quark_gpio, "Galileo Quark GPIO driver", &mod_ops, {"pci"});


