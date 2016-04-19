#ifndef _MRAA_I2C__
#define _MRAA_I2C__

#include "syscall.h"
#include <inttypes.h>
#include <stdlib.h>
#include "mraa_types.h"

enum {I2C_INIT, I2C_WRITE_BYTE, I2C_WRITE_WORD, I2C_READ_BYTE_DATA, I2C_READ_WORD_DATA,
	I2C_READ_BYTES_DATA, I2C_WRITE_WORD_DATA};

struct _mraa_i2c_context
{
	int bus;
	uint8_t addr;
};
typedef struct _mraa_i2c_context * mraa_i2c_context;

mraa_i2c_context
mraa_i2c_init(int bus)
{
	mraa_i2c_context ic = malloc(sizeof(struct _mraa_i2c_context));
	ic->bus = bus;
	return ic;
}

mraa_result_t
mraa_i2c_address(mraa_i2c_context ic, uint8_t addr)
{
	ic->addr = addr;
	make_i2c_syscall(I2C_INIT, addr, 0, 0);
	return MRAA_SUCCESS;
}

mraa_result_t
mraa_i2c_write_byte(mraa_i2c_context ic, uint8_t data)
{
	if (make_i2c_syscall(I2C_WRITE_BYTE, data, 0, 0))
		return MRAA_ERROR;
	else 
		return MRAA_SUCCESS;
}

mraa_result_t
mraa_i2c_write_word(mraa_i2c_context ic, uint16_t data)
{
	if (make_i2c_syscall(I2C_WRITE_WORD, data, 0, 0))
		return MRAA_ERROR;
	else 
		return MRAA_SUCCESS;
}

mraa_result_t
mraa_i2c_write_word_data(mraa_i2c_context ic, uint16_t data, uint8_t cmd)
{
	if (make_i2c_syscall(I2C_WRITE_WORD_DATA, data, cmd, 0))
		return MRAA_ERROR;
	else 
		return MRAA_SUCCESS;
}

int
mraa_i2c_read_byte_data(mraa_i2c_context ic, const uint8_t command)
{
	return make_i2c_syscall(I2C_READ_BYTE_DATA, command, 0, 0);
}

int
mraa_i2c_read_word_data(mraa_i2c_context ic, const uint8_t command)
{
	return make_i2c_syscall(I2C_READ_WORD_DATA, command, 0, 0);
}

int
mraa_i2c_read_bytes_data(mraa_i2c_context ic, uint8_t command, 
												 uint8_t *data, int length)
{
	return make_i2c_syscall(I2C_READ_BYTES_DATA, command, (int)data, length);
}

mraa_result_t
mraa_i2c_stop(mraa_i2c_context ic)
{
	free(ic);
	return MRAA_SUCCESS;
}

#endif
