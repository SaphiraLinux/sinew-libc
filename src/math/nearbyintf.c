#include <fenv.h>
#include <math.h>

float nearbyintf(float x)
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
	x = rintf(x);
#ifdef FE_INEXACT
	if (!e)
		feclearexcept(FE_INEXACT);
#endif
	return x;
}
