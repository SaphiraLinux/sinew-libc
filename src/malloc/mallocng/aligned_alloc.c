#include <stdlib.h>
#include <errno.h>
#include <sys/mman.h>
#include "meta.h"

void *aligned_alloc(size_t align, size_t len)
{
	if ((align & -align) != align) {
		errno = EINVAL;
		return 0;
	}

	if (len > SIZE_MAX - align || align >= (1ULL<<31)*UNIT) {
		errno = ENOMEM;
		return 0;
	}

	if (DISABLE_ALIGNED_ALLOC) {
		errno = ENOMEM;
		return 0;
	}

	if (align <= UNIT) align = UNIT;

	unsigned char *p = malloc(len + align - UNIT);
	if (!p)
		return 0;

	struct meta *g = get_meta(p);
	int idx = get_slot_index(p);
	size_t stride = get_stride(g);
	unsigned char *start = g->mem->storage + stride*idx;
	unsigned char *end = g->mem->storage + stride*(idx+1) - IB;
	size_t adj = -(uintptr_t)p & (align-1);

	if (!g->last_idx && g->maplen &&
	    (size_t)(end - p) - len > (size_t)INT_MAX) {
		/* Extreme overalignment can leave gigabytes of headroom
		 * between the (possibly unshifted) payload and the slot
		 * end.  The slot metadata below cannot represent that:
		 * set_size takes an int reservation and the tail field
		 * is u32, so a truncated value corrupts the books and
		 * crashes (2^34 aligned_alloc SIGSEGV, layout-dependent).
		 * The group is dedicated to this chunk (single-slot
		 * classless group; small size-class groups can never
		 * reach this headroom), so return the unneeded tail
		 * pages instead: exact metadata, less waste, same
		 * capability.  Checked before any shift so the adj==0
		 * fast path below is covered too. */
		unsigned char *grp = (unsigned char *)g->mem;
		size_t keep = (size_t)((p + adj + len + IB + UNIT) - grp);
		keep += -(intptr_t)keep & (PGSZ - 1);
		size_t old = (size_t)g->maplen * 4096UL;
		if (keep < old) {
			if (munmap(grp + keep, old - keep)) {
				free(p);
				errno = ENOMEM;
				return 0;
			}
			g->maplen = keep / 4096;
			end = g->mem->storage + (keep - UNIT) - IB;
		}
	}

	if (!adj) {
		set_size(p, end, len);
		return p;
	}
	p += adj;
	uint32_t offset = (size_t)(p-g->mem->storage)/UNIT;
	if (offset <= 0xffff) {
		*(uint16_t *)(p-2) = offset;
		p[-4] = 0;
	} else {
		// use a 32-bit offset if 16-bit doesn't fit. for this,
		// 16-bit field must be zero, [-4] byte nonzero.
		*(uint16_t *)(p-2) = 0;
		*(uint32_t *)(p-8) = offset;
		p[-4] = 1;
	}
	p[-3] = idx;
	set_size(p, end, len);
	// store offset to aligned enframing. this facilitates cycling
	// offset and also iteration of heap for debugging/measurement.
	// for extreme overalignment it won't fit but these are classless
	// allocations anyway.
	*(uint16_t *)(start - 2) = (size_t)(p-start)/UNIT;
	start[-3] = 7<<5;
	return p;
}
