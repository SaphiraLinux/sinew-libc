#include <fenv.h>
#include <math.h>

/* nearbyint is the same as rint, but it must not raise the inexact exception */

double nearbyint(double x)
{
#ifdef FE_INEXACT
	/* Retained for compilers implementing STDC FENV_ACCESS; GCC
	 * does not, so silence only its unknown-pragma complaint here. */
	#pragma GCC diagnostic push
	#pragma GCC diagnostic ignored "-Wunknown-pragmas"
	#pragma STDC FENV_ACCESS ON
	#pragma GCC diagnostic pop
	int e;

	e = fetestexcept(FE_INEXACT);
#endif
	x = rint(x);
#ifdef FE_INEXACT
	if (!e)
		feclearexcept(FE_INEXACT);
#endif
	return x;
}
