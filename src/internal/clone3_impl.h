#ifndef CLONE3_IMPL_H
#define CLONE3_IMPL_H

#include <stddef.h>

/*
 * Sinew clone3 internal interface (P1 mechanism, P2 consumers).
 *
 * struct clone_args mirrors the stable Linux clone_args ABI (fields
 * are append-only); it lives here, not in a kernel header, because
 * the sinew build closure carries no kernel UAPI headers.
 *
 * Calling convention for the raw field set: the low signal byte is
 * NOT part of flags. Callers split any legacy signal out of the
 * flags word into exit_signal (fork-like: flags 0, exit SIGCHLD;
 * pthread-like: exit_signal 0; public clone/posix_spawn: signal
 * from the flags low byte). __clone3_launch and __clone3_dispatch
 * strip CLONE_DETACHED internally; callers pass their flags whole.
 */
struct clone_args {
	unsigned long long flags;
	unsigned long long pidfd;
	unsigned long long child_tid;
	unsigned long long parent_tid;
	unsigned long long exit_signal;
	unsigned long long stack;
	unsigned long long stack_size;
	unsigned long long tls;
	unsigned long long set_tid;
	unsigned long long set_tid_size;
	unsigned long long cgroup;
};

hidden long __clone3(struct clone_args *args, size_t size);
hidden long __clone3_launch(struct clone_args *args, size_t size,
	int (*func)(void *), void *arg);
hidden long __clone3_dispatch(struct clone_args *args, size_t size,
	int (*func)(void *), void *arg);

/* Dispatch diagnostics (Sinew 0042). Process-local monotonic
 * counters, updated with atomic increments (no locks): the cost is
 * one locked increment per dispatch decision against a syscall that
 * costs hundreds of cycles. Readers copy a consistent-enough
 * snapshot; exact cross-thread simultaneity is not promised.
 * Accounting per call: exactly one of live, fallback_enosys,
 * fallback_eperm, einval or refused advances, except a legacy
 * attempt that itself fails still counts under its trigger class
 * while the negative return propagates to the caller. Counters wrap
 * on overflow (practically unreachable); they never reset. */
struct clone3_diag {
	int attempts;
	int live;
	int fallback_enosys;
	int fallback_eperm;
	int einval;
	int refused;
};

hidden void __clone3_diag(struct clone3_diag *out);

#endif
