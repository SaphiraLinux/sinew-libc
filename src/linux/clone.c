#define _GNU_SOURCE
#include <stdarg.h>
#include <unistd.h>
#include <sched.h>
#include "pthread_impl.h"
#include "clone3_impl.h"
#include "syscall.h"
#include "lock.h"
#include "fork_impl.h"

struct clone_start_args {
	int (*func)(void *);
	void *arg;
	sigset_t sigmask;
};

static int clone_start(void *arg)
{
	struct clone_start_args *csa = arg;
	__post_Fork(0);
	__restore_sigs(&csa->sigmask);
	return csa->func(csa->arg);
}

int clone(int (*func)(void *), void *stack, int flags, void *arg, ...)
{
	struct clone_start_args csa;
	struct clone_args c3a = { 0 };
	va_list ap;
	pid_t *ptid = 0, *ctid = 0;
	void  *tls = 0;

	/* Flags that produce an invalid thread/TLS state are disallowed. */
	int badflags = CLONE_THREAD | CLONE_SETTLS | CLONE_CHILD_CLEARTID;

	if ((flags & badflags) || !stack)
		return __syscall_ret(-EINVAL);

	va_start(ap, arg);
	if (flags & (CLONE_PIDFD | CLONE_PARENT_SETTID | CLONE_CHILD_SETTID))
	 	ptid = va_arg(ap, pid_t *);
	if (flags & CLONE_CHILD_SETTID) {
		tls = va_arg(ap, void *);
		ctid = va_arg(ap, pid_t *);
	}
	va_end(ap);

	/* clone3 takes the stack as base+size; the legacy wrapper only
	 * knows the top. Only the sum matters to the kernel
	 * (rsp = base+size), so describe an 8-byte window ending at
	 * the top: this touches exactly the same slot the legacy
	 * trampoline pushes its argument to, with identical fault
	 * behavior for bogus stacks. The signal travels in
	 * exit_signal, never in the flags word. */
	c3a.flags = (unsigned long long)(flags & ~0xff);
	c3a.exit_signal = (unsigned long long)(flags & 0xff);
	c3a.stack = (unsigned long long)(uintptr_t)stack - 8;
	c3a.stack_size = 8;
	c3a.tls = (unsigned long long)(uintptr_t)tls;
	c3a.parent_tid = (unsigned long long)(uintptr_t)ptid;
	c3a.child_tid = (unsigned long long)(uintptr_t)ctid;

	/* If CLONE_VM is used, it's impossible to give the child a consistent
	 * thread structure. In this case, the best we can do is assume the
	 * caller is content with an extremely restrictive execution context
	 * like the one vfork() would provide. */
	if (flags & CLONE_VM) return __syscall_ret(
		__clone3_dispatch(&c3a, sizeof c3a, func, arg));

	__block_all_sigs(&csa.sigmask);
	LOCK(__abort_lock);

	/* Setup the a wrapper start function for the child process to do
	 * mimic _Fork in producing a consistent execution state. */
	csa.func = func;
	csa.arg = arg;
	int ret = __clone3_dispatch(&c3a, sizeof c3a, clone_start, &csa);

	__post_Fork(ret);
	__restore_sigs(&csa.sigmask);
	return __syscall_ret(ret);
}
