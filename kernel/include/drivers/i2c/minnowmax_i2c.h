#include "kernel.h"

u8 byt_i2c_read_byte_data(u8 command);
s32 byt_i2c_write_byte_data(u8 value);
s32 byt_i2c_write_word_data(u16 value);
void byt_i2c_xfer_init(u32 slave_addr);
int byt_i2c_read_bytes_data(u8, u8 *, u32);

