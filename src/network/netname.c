#include <netdb.h>

struct netent *getnetbyaddr(uint32_t net, int type)
{
	(void)net;
	(void)type;
	return 0;
}

struct netent *getnetbyname(const char *name)
{
	(void)name;
	return 0;
}

