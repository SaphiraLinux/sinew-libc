#include "pthread_impl.h"
#include <threads.h>

int thrd_create(thrd_t *thr, thrd_start_t func, void *arg)
{
	/* Same function-pointer conversion as the pthread start
	 * wrappers: the child entry consumes the low 32 bits as its
	 * exit status, matching the legacy __clone trampoline.
	 * Narrowly scoped. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
	int ret = __pthread_create(thr, __ATTRP_C11_THREAD, (void *(*)(void *))func, arg);
#pragma GCC diagnostic pop
	switch (ret) {
	case 0:      return thrd_success;
	case EAGAIN: return thrd_nomem;
	default:     return thrd_error;
	}
}
