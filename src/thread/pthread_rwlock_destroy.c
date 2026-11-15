#include "pthread_impl.h"

int pthread_rwlock_destroy(pthread_rwlock_t *rw)
{
	(void)rw;
	return 0;
}
