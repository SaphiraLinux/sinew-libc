#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <stddef.h>
#include <string.h>
#include "syscall.h"
#include "clone3_impl.h"

/*
 * Public clone3(2) wrapper (Sinew P3). Thin validation layer over
 * the internal launch; every fallback/error policy lives in
 * __clone3_launch (src/linux/clone3.c), never here.
 *
 * This is the explicit-stack callback form only: the child runs
 * cb(arg) on the supplied stack and exits with cb's return value
 * as its exit status; the parent returns the child pid. There is
 * no return-twice form here: a NULL callback is EINVAL, always.
 * (f2b6525 briefly allowed NULL-callback inherit semantics; that
 * was a unilateral deviation from the agreed contract and is
 * withdrawn. If inherit semantics are wanted, that is a separate
 * deliberate ABI decision, not a side effect of this wrapper.)
 *
 * Size discipline: clone3(2) versions its struct at 64, 80 and 88
 * bytes, and the kernel reads only the supplied prefix. The
 * wrapper does the same: anything below 64 is EINVAL outright
 * (the kernel accepts nothing smaller), and only
 * min(size, 88) bytes are copied. Fields past the supplied size
 * stay zero. In particular a size-64 caller that happens to have
 * a nonzero cgroup member gets launch semantics, not a refusal:
 * the field was never supplied.
 *
 * Signal discipline: the exit signal travels in exit_signal only.
 * A nonzero low byte in flags is EINVAL, not translated: silent
 * translation would bless a usage the strict clone3 validation
 * rejects, and two sources of truth need an owner. (The same
 * reasoning withdrew the f2b6525 translation.)
 *
 * CLONE_DETACHED is stripped before dispatch: a historical no-op
 * bit the kernel rejects with EINVAL. No other flag is touched.
 *
 * Failure contract (from the launcher, unchanged): ENOSYS caches
 * the number dead and falls back to legacy clone; EPERM falls back
 * without caching (seccomp may deny the number, not the ability);
 * EINVAL propagates and never falls back. Anything requesting
 * pidfd/cgroup/set_tid that cannot be served fails loudly with
 * ENOSYS instead of silently dropping the property.
 */
pid_t clone3(struct clone3_args *args, size_t size,
	int (*cb)(void *), void *arg)
{
	struct clone_args a;
	size_t n;
	long ret;

	if (!args || size < 64) {
		errno = EINVAL;
		return -1;
	}
	if (!cb) {
		errno = EINVAL;
		return -1;
	}
	memset(&a, 0, sizeof a);
	n = size < sizeof a ? size : sizeof a;
	memcpy(&a, args, n);

	if (a.flags & 0xffull) {
		errno = EINVAL;
		return -1;
	}
	if (!a.stack || !a.stack_size) {
		errno = EINVAL;
		return -1;
	}

	ret = __clone3_launch(&a, size, cb, arg);
	return __syscall_ret(ret);
}
