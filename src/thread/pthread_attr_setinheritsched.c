#include "pthread_impl.h"
#include "syscall.h"

int pthread_attr_setinheritsched(pthread_attr_t *a, int inherit)
{
	if ((unsigned)inherit > 1U) return EINVAL;
	a->_a_sched = inherit;
	return 0;
}
