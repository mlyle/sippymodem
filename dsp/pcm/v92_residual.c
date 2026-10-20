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
	MOMENT_COLUMNS = V92_RESIDUAL_LAGS + 2,
	MOMENTS = MOMENT_COLUMNS * (MOMENT_COLUMNS + 1) / 2,
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
	/* Every candidate uses columns from the same acquisition window. These
	 * integer products and 120-sample sums are exact in binary64, so sharing
	 * the lower triangle preserves each candidate's normal equations. */
	double moments[MOMENTS] = { 0 };
	double cross[MOMENT_COLUMNS] = { 0 };
	for (unsigned n = START; n < SAMPLES; n++) {
		double row[MOMENT_COLUMNS];
		for (unsigned lag = 0; lag <= V92_RESIDUAL_LAGS; lag++)
			row[lag] = points[n - lag];
		row[MOMENT_COLUMNS - 1] = 1.;
		gram_accumulate(moments, cross, row, MOMENT_COLUMNS, raw[n]);
	}
	struct v92_residual best = { 0 };
	double best_gain = 0., best_score = INFINITY;
	const unsigned samples = SAMPLES - START;
	for (unsigned first = 0; first <= V92_RESIDUAL_LAGS; first++)
		for (unsigned second = 0; second <= V92_RESIDUAL_LAGS; second++) {
			if (second && (!first || second <= first)) continue;
			unsigned taps = (first != 0) + (second != 0), count = taps + 2;
			unsigned columns[4] = { 0 }, used = 1;
			if (first) columns[used++] = first;
			if (second) columns[used++] = second;
			columns[used] = MOMENT_COLUMNS - 1;
			double gram[GRAM_FIT_MOMENTS] = { 0 }, rhs[4] = { 0 }, fitted[4];
			for (unsigned j = 0; j < count; j++) {
				rhs[j] = cross[columns[j]];
				for (unsigned k = 0; k <= j; k++)
					gram[gram_moment_index(j, k)] = moments[gram_moment_index(columns[j], columns[k])];
			}
			if (!gram_fit(gram, rhs, count, fitted)) continue;
			/* Constrain the main gain, then refit the other coefficients at
			 * that boundary. Discarding the entire model here could select a
			 * wrong delay merely because its unconstrained gain was in range. */
			if (fitted[0] < 1. / maximum || fitted[0] > 1. / minimum) {
				double fixed = fmax(1. / maximum, fmin(1. / minimum, fitted[0]));
				double reduced[GRAM_FIT_MOMENTS] = { 0 }, target[4], other[4];
				for (unsigned i = 1; i < count; i++) {
					target[i - 1] = rhs[i] - gram[gram_moment_index(i, 0)] * fixed;
					for (unsigned j = 1; j <= i; j++)
						reduced[gram_moment_index(i - 1, j - 1)] = gram[gram_moment_index(i, j)];
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

bool v92_residual_translate(struct v92_residual *filter,
	const int32_t *original, const int32_t *translated, const int32_t *raw,
	double gain)
{
	if (!filter || !original || !translated || !raw ||
	    !v92_residual_is_valid(filter) || !filter->active || filter->cursor ||
	    !isfinite(gain) || gain <= 0.) return false;
	const unsigned start = START - V92_RESIDUAL_LAGS;
	int64_t shift = (int64_t)translated[start] - original[start];
	for (unsigned i = start + 1u; i < SAMPLES; ++i)
		if ((int64_t)translated[i] - original[i] != shift) return false;
	struct v92_residual next = *filter;
	double response = 1.;
	for (unsigned i = 0; i < next.count; ++i) response += next.coefficient[i];
	next.offset -= shift * response / gain;
	for (unsigned i = 0; i < V92_RESIDUAL_LAGS; ++i)
		next.history[i] = raw[SAMPLES - V92_RESIDUAL_LAGS + i] - next.offset;
	if (!v92_residual_is_valid(&next)) return false;
	*filter = next;
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
