#include <time.h>
#include <unistd.h>
#include <inttypes.h>
#include <stdio.h>

#define uRdtsc(var)                                            \
 {                                                             \
   unsigned long var##_lo, var##_hi;                           \
   asm volatile("rdtsc" : "=a"(var##_lo), "=d"(var##_hi));     \
   var = var##_hi;                                             \
   var <<= 32;                                                 \
   var |= var##_lo;                                            \
 }

void print_long_long_hex(unsigned long long int val)
{
	unsigned long val_lo = (unsigned long)val;
	unsigned long val_hi = (unsigned long)(val >> 32);

	if (val_hi != 0) {
		printf("0x%lx", val_hi);
		printf("%.8lx\n", val_lo);
	} else {
		printf("0x%lx\n", val_lo);
	}
}

int main()
{
	struct timespec t = {.tv_sec = 0, .tv_nsec = 500000}; /* 500ms */
	uint64_t start, end;

	uRdtsc(start);
	nanosleep(&t, NULL);
	//usleep(500);
	uRdtsc(end);

	print_long_long_hex(start);
	print_long_long_hex(end);

	return 0;
}
