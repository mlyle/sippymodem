// SPDX-License-Identifier: BSD-3-Clause
#ifndef GAUSSIAN_H
#define GAUSSIAN_H

#include <fenv.h>
#include <stdbool.h>
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

/* Conditional mean and variance inside [low, high]; bounds may be infinite.
 * The caller supplies a finite mean, positive finite variance and its positive
 * square root sigma, computed once for a batch. Distance outside the interval
 * alone underestimates noise even when a prediction falls inside the bin.
 * Output pointers must be valid
 * and distinct. On failure discard both outputs; no pointer is retained. */
bool gaussian_interval_moments(double low, double high, double mean, double variance,
	double sigma, double *expected, double *deviation);

#endif /* GAUSSIAN_H */
