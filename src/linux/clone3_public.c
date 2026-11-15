#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <stddef.h>
#include "syscall.h"
#include "clone3_impl.h"

/*
 * Public clone3(2) wrapper (Sinew P3). Thin errno-translating layer
 * over the internal dispatch; every policy lives in
 * __clone3_launch/__clone3_dispatch (src/linux/clone3.c), never here.
 *
 * Two execution models, selected by the callback:
 *
 * - cb != NULL: explicit-stack form. args->stack/stack_size must
 *   describe the child stack; the child runs cb(arg) on it and exits
 *   with cb's return value as its exit status. The parent returns the
 *   child pid. Missing stack with a callback is EINVAL: after a stack
 *   switch no C call can return, so the raw inherit form cannot serve
 *   a callback (see __clone3 in clone3.c).
 *
 * - cb == NULL: inherit form, returns twice like fork()/clone().
 *   args->stack/stack_size must both be zero: an explicit stack with
 *   no callback would strand the child with nowhere to return, and
 *   the kernel accepts the combination silently, so this is EINVAL
 *   here instead of a stillborn child.
 *
 * Signal: the exit signal may arrive either as the legacy low byte
 * of flags (clone(2) style, e.g. flags|SIGCHLD) or as exit_signal.
 * If both are given they must agree, else EINVAL; the effective
 * value goes to exit_signal and the flags low byte is cleared,
 * because clone3(2) validates flags strictly and rejects the byte.
 *
 * Failure contract (from the dispatcher, unchanged): ENOSYS caches
 * the number dead and falls back to legacy clone; EPERM falls back
 * without caching (seccomp may deny the number, not the ability);
 * EINVAL propagates and never falls back. Anything requesting
 * pidfd/cgroup/set_tid that cannot be served fails loudly with
 * ENOSYS instead of silently dropping the property. CLONE_DETACHED
 * is stripped: a historical no-op the kernel rejects with EINVAL.
 *
 * struct clone3_args mirrors the stable Linux clone_args ABI
 * (fields append-only, 11 x u64); it is copied field by field into
 * the internal struct so the two can never silently diverge.
 */
pid_t clone3(struct clone3_args *args, size_t size,
	int (*cb)(void *), void *arg)
{
	struct clone_args a;
	unsigned long long sig;
	long ret;

	if (!args) {
		errno = EINVAL;
		return -1;
	}
	a.flags = args->flags;
	a.pidfd = args->pidfd;
	a.child_tid = args->child_tid;
	a.parent_tid = args->parent_tid;
	a.exit_signal = args->exit_signal;
	a.stack = args->stack;
	a.stack_size = args->stack_size;
	a.tls = args->tls;
	a.set_tid = args->set_tid;
	a.set_tid_size = args->set_tid_size;
	a.cgroup = args->cgroup;

	sig = a.flags & 0xffull;
	a.flags &= ~0xffull;
	if (sig && a.exit_signal && sig != (a.exit_signal & 0xffull)) {
		errno = EINVAL;
		return -1;
	}
	if (!a.exit_signal)
		a.exit_signal = sig;

	if (!cb) {
		if (a.stack || a.stack_size) {
			errno = EINVAL;
			return -1;
		}
	} else {
		if (!a.stack || !a.stack_size) {
			errno = EINVAL;
			return -1;
		}
	}

	ret = __clone3_dispatch(&a, size, cb, arg);
	return __syscall_ret(ret);
}
