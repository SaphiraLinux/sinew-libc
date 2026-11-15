#include <wchar.h>

int wcsncasecmp_l(const wchar_t *l, const wchar_t *r, size_t n, locale_t locale)
{
	(void)locale;
	return wcsncasecmp(l, r, n);
}
