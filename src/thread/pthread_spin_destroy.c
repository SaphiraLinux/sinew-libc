#include "pthread_impl.h"

int pthread_spin_destroy(pthread_spinlock_t *s)
{
	(void)s;
	return 0;
}
