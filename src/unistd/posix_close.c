#include <unistd.h>

int posix_close(int fd, int flags)
{
	(void)flags;
	return close(fd);
}
