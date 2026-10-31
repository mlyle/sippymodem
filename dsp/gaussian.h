// SPDX-License-Identifier: BSD-3-Clause
#ifndef GAUSSIAN_H
#define GAUSSIAN_H

#include <fenv.h>
#include <math.h>

/* Unit-normal density at x standard deviations. Unlike the interval
 * probability arguments below, x has not been divided by sqrt(2). Keep
 * the shared E/M and Newton fits on the same density arithmetic. */
static inline double gaussian_density(double x)
{
	return exp(-.5 * x * x) / sqrt(2. * acos(-1.));
}

/* On one side of zero, erfc(b)/erfc(a) <= exp(a*a-b*b).
 * A gap of 40 puts the subtracted tail below half an ulp of erfc(a),
 * including subnormal results. Directed rounding keeps the subtraction. */
static inline double gaussian_positive_probability(double a, double b)
{
	if (a >= 28.) return 0.;
	double tail = erfc(a);
	if (b >= 28. || ((b - a) * (b + a) >= 40. && fegetround() == FE_TONEAREST))
		return .5 * tail;
	return .5 * (tail - erfc(b));
}

/* Gaussian interval probability with arguments scaled by sqrt(2)*sigma.
 * erfc(28) is below half the smallest binary64 subnormal; erf(6) rounds
 * to one. These are representable-value tails, not probability cutoffs. */
static inline double gaussian_probability(double a, double b)
{
	if (a > 0.) return gaussian_positive_probability(a, b);
	if (b < 0.) return gaussian_positive_probability(-b, -a);
	return .5 * ((b >= 6. ? 1. : erf(b)) - (a <= -6. ? -1. : erf(a)));
}

/* Logarithm of the same interval mass, retaining finite ordering when
 * probability underflows. Arguments are still scaled by sqrt(2)*sigma.
 * Requires a <= b; bounds may be infinite for one-sided clipping. A
 * nonfinite result signals an empty interval or unresolved numeric tail;
 * check it before converting a scaled cost to an integer. */
double gaussian_log_probability(double a, double b);

#endif /* GAUSSIAN_H */
