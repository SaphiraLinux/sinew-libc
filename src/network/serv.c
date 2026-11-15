#include <netdb.h>

void endservent(void)
{
}

void setservent(int stayopen)
{
	(void)stayopen;
}

struct servent *getservent(void)
{
	return 0;
}
