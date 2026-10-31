// SPDX-License-Identifier: BSD-3-Clause
#ifndef INTERVAL_FIT_H
#define INTERVAL_FIT_H

#include "gaussian.h"
#include "gram_fit.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { INTERVAL_FIT_SAMPLES = 288 };

/* Conditional moments of a Gaussian inside the received codec interval.
 * Measuring only distance OUTSIDE the interval underestimates noise: a
 * prediction inside the bin is not evidence that no noise was present. */
static inline bool interval_fit_moments(double low, double high, double mean, double variance,
	double sigma, double *expected, double *deviation)
{
	double a = (low - mean) / sigma, b = (high - mean) / sigma;
	double probability = gaussian_probability(a / sqrt(2.), b / sqrt(2.));
	if (!(probability > 0.)) return false;
	double pa = exp(-.5 * a * a) / sqrt(2. * acos(-1.));
	double pb = exp(-.5 * b * b) / sqrt(2. * acos(-1.));
	double shift = (pa - pb) / probability;
	double ap = isfinite(a) ? a * pa : 0., bp = isfinite(b) ? b * pb : 0.;
	*expected = mean + sigma * shift;
	*deviation = variance * fmax(0., 1. + (ap - bp) / probability - shift * shift);
	return isfinite(*expected) && isfinite(*deviation);
}

/* One bounded conditional-bin E/M update over delayed integer features.
 * The owner supplies its fixed packed Gram system, coefficient/noise seed,
 * and received intervals. Fit 1..4 taps over [first,count), with preceding
 * feature history available: first >= taps-1 and count <= 288.
 * Bounds may be infinite for clipped cells. Noise is in sample-squared units
 * and must be finite and at least one; the same floor applies to the result.
 *
 * Arrays cover their stated sample/feature counts. All storage is separate,
 * except next may alias coefficient. No pointer is retained. Failure writes
 * neither next nor variance; success writes four coefficients (unused zero).
 * Admission, iteration count, convergence and scheduling belong to the owner.
 * The work is at most 288 observations and one four-column Gram solve. */
static inline bool interval_fit_step(const double coefficient[GRAM_FIT_COLUMNS],
	const int16_t *q, const double *low, const double *high,
	size_t count, size_t first, unsigned taps,
	const double gram[GRAM_FIT_MOMENTS], double noise,
	double next[GRAM_FIT_COLUMNS], double *variance)
{
	if (!coefficient || !q || !low || !high || !gram || !next || !variance ||
	    !taps || taps > GRAM_FIT_COLUMNS || first < taps - 1u || first >= count ||
	    count > INTERVAL_FIT_SAMPLES || !isfinite(noise) || noise < 1.) return false;
	double targets[INTERVAL_FIT_SAMPLES], deviations[INTERVAL_FIT_SAMPLES];
	double rhs[GRAM_FIT_COLUMNS] = { 0 }, fitted[GRAM_FIT_COLUMNS] = { 0 }, next_noise = 0.;
	/* Every bin in this update uses the same noise distribution. */
	double sigma = sqrt(noise);
	for (size_t j = first; j < count; j++) {
		double mean = 0.;
		for (unsigned i = 0; i < taps; i++) mean += coefficient[i] * q[j - i];
		if (!interval_fit_moments(low[j], high[j], mean, noise, sigma,
			targets + j, deviations + j)) return false;
		for (unsigned i = 0; i < taps; i++) rhs[i] += q[j - i] * targets[j];
	}
	if (taps == 1u) {
		fitted[0] = rhs[0] / gram[0];
		if (!isfinite(fitted[0]) || fitted[0] <= 0.) return false;
	} else if (!gram_fit(gram, rhs, taps, fitted) || fitted[0] <= 0.) return false;
	for (size_t j = first; j < count; j++) {
		double residual = targets[j];
		for (unsigned i = 0; i < taps; i++) residual -= fitted[i] * q[j - i];
		next_noise += deviations[j] + residual * residual;
	}
	next_noise = fmax(1., next_noise / (count - first));
	if (!isfinite(next_noise)) return false;
	memcpy(next, fitted, sizeof(fitted));
	*variance = next_noise;
	return true;
}

#endif /* INTERVAL_FIT_H */
