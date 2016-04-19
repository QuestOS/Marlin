#include "kernel.h"

int byt_i2c_read_byte_data(u8);
int byt_i2c_read_word_data(u8);
int byt_i2c_read_bytes_data(u8, u8 *, u32);
s32 byt_i2c_write_byte(u8);
s32 byt_i2c_write_word(u16);
s32 byt_i2c_write_word_data(u8, u16);
void byt_i2c_xfer_init(u32 slave_addr);

