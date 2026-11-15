#include <spawn.h>

int posix_spawnattr_destroy(posix_spawnattr_t *attr)
{
	(void)attr;
	return 0;
}
