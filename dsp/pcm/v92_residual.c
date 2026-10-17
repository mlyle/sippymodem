// SPDX-License-Identifier: BSD-3-Clause
#include "v92_residual.h"
#include "memory_region.h"
#include "gram_fit.h"

#include <math.h>
#include <string.h>

enum {
	SAMPLES = V92_B1U_ACQUISITION_FRAMES * V92_PCM_MAPPING_FRAME_SYMBOLS,
	START = (V92_B1U_ACQUISITION_FRAMES - V92_PCM_RX_DELAY_FRAMES) *
		V92_PCM_MAPPING_FRAME_SYMBOLS,
	/* Small physical DC biases can exceed the data error ceiling. This is
	 * a fit bound, not an increase to the held-out residual/bit limits. */
	MAX_OFFSET = 512,
};
_Static_assert((unsigned)START >= (unsigned)V92_RESIDUAL_LAGS, "acquisition must include delayed predictors");

bool v92_residual_is_valid(const struct v92_residual *filter)
{
	if (!filter) return false;
	if (!filter->active)
		return memory_region_is_zero(filter, sizeof(*filter));
	if (filter->count > 2 || filter->cursor >= V92_RESIDUAL_LAGS ||
	    !isfinite(filter->offset) || fabs(filter->offset) > MAX_OFFSET)
		return false;
	double norm = 0.;
	for (unsigned i = 0; i < 2; i++) {
		if (!isfinite(filter->coefficient[i])) return false;
		if (i < filter->count) {
			if (!filter->delay[i] || filter->delay[i] > V92_RESIDUAL_LAGS ||
			    (i && filter->delay[i] <= filter->delay[i - 1])) return false;
			norm += fabs(filter->coefficient[i]);
		} else if (filter->delay[i] || filter->coefficient[i]) return false;
	}
	for (unsigned i = 0; i < V92_RESIDUAL_LAGS; i++)
		if (!isfinite(filter->history[i]) || fabs(filter->history[i]) > 32768.)
			return false;
	return norm <= .10;
}

static void features(const int32_t *points, unsigned n,
	unsigned first, unsigned second, double x[4])
{
	unsigned k = 0;
	x[k++] = points[n];
	if (first) x[k++] = points[n - first];
	if (second) x[k++] = points[n - second];
	x[k] = 1.;
}

bool v92_residual_fit(struct v92_residual *filter,
	const int32_t *points, const int32_t *raw, double minimum, double maximum,
	double *gain, double *score)
{
	if (!filter || !points || !raw || !gain || !isfinite(minimum) ||
	    !isfinite(maximum) || minimum <= 0. || maximum < minimum) return false;
	for (unsigned i = 0; i < SAMPLES; i++)
		if (points[i] < -32768 || points[i] > 32768 || raw[i] < -32768 || raw[i] > 32768)
			return false;
	struct v92_residual best = { 0 };
	double best_gain = 0., best_score = INFINITY;
	const unsigned samples = SAMPLES - START;
	for (unsigned first = 0; first <= V92_RESIDUAL_LAGS; first++)
		for (unsigned second = 0; second <= V92_RESIDUAL_LAGS; second++) {
			if (second && (!first || second <= first)) continue;
			unsigned taps = (first != 0) + (second != 0), count = taps + 2;
			double gram[4][4] = { { 0 } }, rhs[4] = { 0 }, fitted[4];
			for (unsigned n = START; n < SAMPLES; n++) {
				double x[4]; features(points, n, first, second, x);
				for (unsigned j = 0; j < count; j++) {
					for (unsigned k = 0; k < count; k++) gram[j][k] += x[j] * x[k];
					rhs[j] += x[j] * raw[n];
				}
			}
			if (!gram_fit(gram, rhs, count, fitted)) continue;
			/* Constrain the main gain, then refit the other coefficients at
			 * that boundary. Discarding the entire model here could select a
			 * wrong delay merely because its unconstrained gain was in range. */
			if (fitted[0] < 1. / maximum || fitted[0] > 1. / minimum) {
				double fixed = fmax(1. / maximum, fmin(1. / minimum, fitted[0]));
				double reduced[4][4] = { { 0 } }, target[4], other[4];
				for (unsigned i = 1; i < count; i++) {
					target[i - 1] = rhs[i] - gram[i][0] * fixed;
					for (unsigned j = 1; j < count; j++) reduced[i - 1][j - 1] = gram[i][j];
				}
				if (!gram_fit(reduced, target, count - 1, other)) continue;
				fitted[0] = fixed;
				for (unsigned i = 1; i < count; i++) fitted[i] = other[i - 1];
			}
			struct v92_residual candidate = { .active = true, .count = taps,
				.delay = { first, second }, .offset = fitted[count - 1] };
			for (unsigned j = 0; j < taps; j++) candidate.coefficient[j] = fitted[j + 1] / fitted[0];
			for (unsigned i = 0; i < V92_RESIDUAL_LAGS; i++)
				candidate.history[i] = raw[SAMPLES - V92_RESIDUAL_LAGS + i] - candidate.offset;
			if (!v92_residual_is_valid(&candidate)) continue;
			double error = 0.;
			for (unsigned n = START; n < SAMPLES; n++) {
				double x[4], residual = raw[n]; features(points, n, first, second, x);
				for (unsigned j = 0; j < count; j++) residual -= fitted[j] * x[j];
				error += residual * residual;
			}
			/* BIC plus a delay-search penalty favors scalar/one-delay fits
			 * unless another coefficient explains acquisition substantially.
			 * Held-out B1u is never consulted to select the model. */
			double score = samples * log(fmax(1e-12, error / samples)) +
				count * log(samples) + 2 * taps * log(V92_RESIDUAL_LAGS);
			if (isfinite(score) && score < best_score) {
				best = candidate; best_gain = fmax(minimum, fmin(maximum, 1. / fitted[0])); best_score = score;
			}
		}
	if (!best.active) return false;
	*filter = best; *gain = best_gain;
	if (score) *score = best_score;
	return true;
}

bool v92_residual_sample(struct v92_residual *filter,
	int32_t raw, int polarity, bool available, int32_t *correction)
{
	if (!filter->active) { *correction = 0; return true; }
	double corrected = raw - filter->offset;
	for (unsigned j = 0; j < filter->count; j++)
		corrected -= filter->coefficient[j] * filter->history[
			(filter->cursor + V92_RESIDUAL_LAGS - filter->delay[j]) % V92_RESIDUAL_LAGS];
	double physical = corrected * polarity;
	if (!available || !isfinite(physical) || physical < INT16_MIN || physical > INT16_MAX)
		return false;
	filter->history[filter->cursor] = corrected;
	filter->cursor = (filter->cursor + 1) % V92_RESIDUAL_LAGS;
	*correction = raw - (int32_t)lrint(physical) * polarity;
	return true;
}
