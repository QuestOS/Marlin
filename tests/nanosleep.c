#include <sys/time.h>

int main()
{
	struct timespec t = {.tv_sec = 1, .tv_nsec = 600};

	nanosleep(&t, NULL);

	return 0;
}
