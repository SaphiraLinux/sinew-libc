#include "pthread_impl.h"

int pthread_attr_setdetachstate(pthread_attr_t *a, int state)
{
	if ((unsigned)state > 1U) return EINVAL;
	a->_a_detach = state;
	return 0;
}
