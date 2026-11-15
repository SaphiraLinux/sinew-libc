#include <sched.h>
#include <errno.h>
#include "syscall.h"

int sched_getscheduler(pid_t pid)
{
	(void)pid;
	return __syscall_ret(-ENOSYS);
}
