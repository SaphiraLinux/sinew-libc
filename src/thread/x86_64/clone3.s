.text
.global __clone3_entry
.hidden __clone3_entry
.type __clone3_entry,@function
/* Explicit-stack child entry (Sinew P1). Mirrors clone.s discipline:
 * the parent frame is never switched; only the child resumes on the
 * new stack. On entry: rdi=args, rsi=size, rdx=func, rcx=arg.
 * The kernel sets the child rsp to stack+stack_size, so the argument
 * slot is pushed at top-8 and read back without moving rsp: a C
 * wrapper cannot return through a switched stack, which is why this
 * must be assembly. __clone3_launch guarantees the supplied top is
 * 16-byte aligned (thread stacks are routinely only 8-aligned), so
 * the aligned slot computed here and rsp-8 in the child agree. Raw
 * kernel result: child pid, 0 in the child, or -errno. */
__clone3_entry:
	mov %rdx,%r9
	mov 40(%rdi),%rax
	add 48(%rdi),%rax
	and $-16,%rax
	sub $8,%rax
	mov %rcx,(%rax)
	mov $435,%eax
	syscall
	test %eax,%eax
	jnz 1f
	xor %ebp,%ebp
	mov -8(%rsp),%rdi
	call *%r9
	mov %eax,%edi
	xor %eax,%eax
	mov $60,%al
	syscall
	hlt
1:	ret
