// SPDX-License-Identifier: BSD-3-Clause

#include "gaussian.h"

#include <float.h>

/* log P(N(0,1) > x). Beyond 12 sigma the decreasing asymptotic series
 * reaches binary64 precision before its terms start growing. Working in
 * logs preserves candidate ordering even when the mass underflows. */
static double gaussian_log_tail(double x)
{
	if (x < 12.) return log(.5 * erfc(x / sqrt(2.)));
	double term = 1., sum = 1.;
	for (unsigned i = 1; i < 32; i++) {
		double next = term * (-(double)(2 * i - 1) / (x * x));
		if (fabs(next) > fabs(term)) break;
		sum += next; term = next;
		if (fabs(term) < DBL_EPSILON * fabs(sum)) break;
	}
	return -.5 * x * x - log(x) - .5 * log(2. * acos(-1.)) + log(sum);
}

double gaussian_log_probability(double a, double b)
{
	double probability = gaussian_probability(a, b);
	if (probability >= DBL_MIN) return log(probability);
	if (b < 0.) { double swap = a; a = -b; b = -swap; }
	if (a == HUGE_VAL) return -HUGE_VAL;
	/* Arguments above are scaled by sqrt(2)*sigma, unlike log_tail. */
	double low = gaussian_log_tail(a * sqrt(2.));
	/* Q(sqrt(2)*b)/Q(sqrt(2)*a) <= exp(a*a-b*b) for a >= 0.
	 * Below exp(-40), expm1 rounds to -1 and its log correction is zero.
	 * Bound the argument so exponent rounding stays well below the gap
	 * margin; huge adjacent arguments can otherwise collapse to one tail.
	 * Directed rounding and nonfinite tails retain the original expression. */
	if (isfinite(low) && a >= 0. && a < 0x1p24 &&
		(b - a) * (b + a) >= 40. && fegetround() == FE_TONEAREST)
		return low;
	double high = gaussian_log_tail(b * sqrt(2.));
	return low + log(-expm1(high - low));
}
