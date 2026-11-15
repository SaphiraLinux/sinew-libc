#include <threads.h>

void cnd_destroy(cnd_t *c)
{
	(void)c;
	/* For private cv this is a no-op */
}
