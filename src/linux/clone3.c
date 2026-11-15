#define _GNU_SOURCE
#include <stddef.h>
#include <sched.h>
#include "syscall.h"
#include "pthread_impl.h"
#include "clone3_impl.h"
#include "atomic.h"

/*
 * Raw clone3(2) primitive (Linux 5.3+). Internal only: there is no
 * public declaration, and the definition below is hidden, so this
 * symbol never leaves the sinew build.
 *
 * Contract (enforced, not merely documented): the child returns
 * normally through the caller's stack, which is only possible when
 * the child inherits a copy of it. Explicit stacks are therefore
 * rejected here with EINVAL instead of being passed on: after a
 * stack switch no C call can return (its return address lives on
 * the abandoned stack), so accepting them would jump to garbage.
 * Callers that need an explicit stack must use the
 * callback-plus-trampoline form (__clone3_launch below), where the
 * child never returns through the caller. The kernel itself accepts
 * the combination silently and produces a stillborn child; this
 * guard exists because the failure mode is silent.
 *
 * The return value is the raw kernel result: child pid, 0 in the
 * child, or -errno. Callers that expose errno semantics apply
 * __syscall_ret themselves. No fallback lives here: ENOSYS and
 * policy decisions belong to the caller (see the sinew clone3
 * design), never silently inside the primitive.
 *
 * struct clone_args is declared in clone3_impl.h (stable kernel
 * ABI, fields append-only) because the sinew build closure carries
 * no kernel UAPI headers.
 */
/* Entry trampoline for explicit stacks (arch/x86_64/clone3.s).
 * P1 is x86_64-only; other arches take the -ENOSYS path below. */
hidden long __clone3_entry(struct clone_args *args, size_t size,
	int (*func)(void *), void *arg);

/* Capability cache: 0 unknown, 1 live, -1 unusable in this
 * environment. Values are idempotent; a torn read only repeats
 * a probe. The cache never claims "old kernel": seccomp can forge
 * ENOSYS on new kernels, so -1 means "unavailable here". */
static int clone3_ok;
/* Dispatch diagnostics (Sinew 0042). See clone3_impl.h for the
 * accounting contract. Updated with a_inc: one locked increment
 * per decision, negligible next to a syscall. */
static int c_attempts, c_live, c_fallback_enosys;
static int c_fallback_eperm, c_einval, c_refused;

hidden long __clone3(struct clone_args *args, size_t size)
{
	if (args->stack || args->stack_size) {
		return -EINVAL;
	}
	return __syscall(SYS_clone3, args, size);
}

/* Compatible fallback is permitted only when the legacy int-flags
 * clone() can carry every requested property. Anything else fails
 * loudly instead of silently dropping a requested security or
 * supervision property. In particular a seccomp EPERM on the clone3
 * number denies the number, not the capability; falling back without
 * new properties preserves every requested semantic. Requesting
 * pidfd/cgroup/set_tid and then falling back would not, so that path
 * never falls back. The checks test flags as well as fields: a zero
 * cgroup with CLONE_INTO_CGROUP set, or CLONE_PIDFD with a null
 * pointer, is still a request. Flag bits above 31 (CLONE_CLEAR_
 * SIGHAND, CLONE_INTO_CGROUP, and any future bit) do not survive
 * the int conversion, and an exit_signal that does not fit the
 * legacy signal byte must not be truncated: all refuse. */
static int legacy_expressible(const struct clone_args *args)
{
	if (args->flags & ~0xffffffffull)
		return 0;
	if (args->flags & ((unsigned long long)CLONE_PIDFD |
	                   (unsigned long long)CLONE_CLEAR_SIGHAND |
	                   (unsigned long long)CLONE_INTO_CGROUP))
		return 0;
	if (args->exit_signal & ~0xffull)
		return 0;
	return !args->pidfd && !args->set_tid &&
	       !args->set_tid_size && !args->cgroup;
}

/* Translate to the legacy clone() flags word (signal in the low
 * byte). CLONE_DETACHED is stripped up front: a historical no-op bit
 * the strict clone3 flag validation rejects with EINVAL (measured). */
static int legacy_flags(const struct clone_args *args)
{
	unsigned long long f;
	f = args->flags & ~((unsigned long long)CLONE_DETACHED);
	f &= ~0xffull;
	f |= args->exit_signal & 0xff;
	return (int)f;
}

hidden long __clone3_launch(struct clone_args *args, size_t size,
	int (*func)(void *), void *arg)
{
#ifndef __x86_64__
	return -ENOSYS;
#else
	struct clone_args a = *args;
	long ret;
	int fb_cause = 1;
	a_inc(&c_attempts);
	if (!a.stack || !a.stack_size) {
		a_inc(&c_einval);
		return -EINVAL;
	}
	a.flags &= ~((unsigned long long)CLONE_DETACHED);
	if (clone3_ok < 0) {
		goto fallback;
	}
	ret = __clone3_entry(&a, size, func, arg);
	if (ret >= 0) {
		if (!clone3_ok) {
			clone3_ok = 1;
		}
		a_inc(&c_live);
		return ret;
	}
	if (ret == -ENOSYS) {
		clone3_ok = -1;
		fb_cause = 1;
		goto fallback;
	}
	if (ret == -EPERM) {
		fb_cause = 2;
		goto fallback;
	}
	if (ret == -EINVAL)
		a_inc(&c_einval);
	return ret;
fallback:
	if (!legacy_expressible(&a)) {
		a_inc(&c_refused);
		return -ENOSYS;
	}
	if (fb_cause == 1)
		a_inc(&c_fallback_enosys);
	else
		a_inc(&c_fallback_eperm);
	return __clone(func,
		(void *)(uintptr_t)(a.stack + a.stack_size),
		legacy_flags(&a), arg,
		(void *)(uintptr_t)a.parent_tid,
		(void *)(uintptr_t)a.tls,
		(void *)(uintptr_t)a.child_tid);
#endif
}

/* Dispatch covering both execution models. Inherit-model callers
 * keep raw fork-like semantics; explicit-stack callers go through
 * the trampoline. No existing consumer is migrated in P1; this entry
 * exists so the mechanism and its fallback are testable before any
 * migration. */
hidden long __clone3_dispatch(struct clone_args *args, size_t size,
	int (*func)(void *), void *arg)
{
	struct clone_args a = *args;
	long ret;
	a.flags &= ~((unsigned long long)CLONE_DETACHED);
	if (!a.stack && !a.stack_size) {
		int fb_cause = 1;
		a_inc(&c_attempts);
		/* Inherit model shares the caller's stack, so a
		 * separately supplied stack is mandatory with
		 * CLONE_VM: without one both would run on the same
		 * stack. Reject loudly instead of corrupting it. */
		if (a.flags & (unsigned long long)CLONE_VM) {
			a_inc(&c_refused);
			return -ENOSYS;
		}
		if (clone3_ok < 0) {
			goto fb_inherit;
		}
		ret = __syscall(SYS_clone3, &a, size);
		if (ret >= 0) {
			if (!clone3_ok) {
				clone3_ok = 1;
			}
			a_inc(&c_live);
			return ret;
		}
		if (ret == -ENOSYS) {
			clone3_ok = -1;
			fb_cause = 1;
			goto fb_inherit;
		}
		if (ret == -EPERM) {
			fb_cause = 2;
			goto fb_inherit;
		}
		if (ret == -EINVAL)
			a_inc(&c_einval);
		return ret;
	fb_inherit:
		if (!legacy_expressible(&a)) {
			a_inc(&c_refused);
			return -ENOSYS;
		}
		if (fb_cause == 1)
			a_inc(&c_fallback_enosys);
		else
			a_inc(&c_fallback_eperm);
		/* Forward the tid/tls properties instead of dropping
		 * them: x86_64 legacy clone takes (flags, stack,
		 * parent_tid, child_tid, tls). A request that named
		 * them must keep them, not silently lose them. */
		return __syscall(SYS_clone,
			(int)((a.flags & ~0xffull) |
			      (a.exit_signal & 0xff)),
			0,
			(long)a.parent_tid,
			(long)a.child_tid,
			(long)a.tls);
	}
	return __clone3_launch(&a, size, func, arg);
}

hidden void __clone3_diag(struct clone3_diag *out)
{
	out->attempts = c_attempts;
	out->live = c_live;
	out->fallback_enosys = c_fallback_enosys;
	out->fallback_eperm = c_fallback_eperm;
	out->einval = c_einval;
	out->refused = c_refused;
}
