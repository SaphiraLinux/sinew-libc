#include <sched.h>
#include <errno.h>
#include "syscall.h"

int sched_setscheduler(pid_t pid, int sched, const struct sched_param *param)
{
	(void)pid;
	(void)sched;
	(void)param;
	return __syscall_ret(-ENOSYS);
}
