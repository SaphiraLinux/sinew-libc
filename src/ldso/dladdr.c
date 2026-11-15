#define _GNU_SOURCE
#include <dlfcn.h>

static int stub_dladdr(const void *addr, Dl_info *info)
{
	(void)addr;
	(void)info;
	return 0;
}

weak_alias(stub_dladdr, dladdr);
