.global memmove
.type memmove,@function
memmove:
	mov %rdi,%rax
	sub %rsi,%rax
	cmp %rdx,%rax
.hidden __memcpy_fwd
	jae __memcpy_fwd
	mov %rdi,%rax
	cmp $16,%rdx
	jb 2f
	movups (%rsi),%xmm1
	sub $16,%rdx
	jbe 1f
3:	movups (%rsi,%rdx),%xmm0
	movups %xmm0,(%rdi,%rdx)
	sub $16,%rdx
	ja 3b
1:	movups %xmm1,(%rdi)
	ret
2:	cmp $8,%edx
	jb 2f
	mov (%rsi),%rcx
	mov -8(%rsi,%rdx),%rsi
	mov %rsi,-8(%rdi,%rdx)
	mov %rcx,(%rdi)
	ret
2:	test %edx,%edx
	jz 1f
3:	dec %edx
	movzbl (%rsi,%rdx),%ecx
	mov %cl,(%rdi,%rdx)
	jnz 3b
1:	ret
