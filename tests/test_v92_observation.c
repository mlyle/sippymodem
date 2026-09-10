// SPDX-License-Identifier: BSD-3-Clause
#include "v92_observation.h"
#include "integer_math.h"

#include <assert.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { TRAINING = V92_OBSERVATION_TRAINING, SAMPLES = TRAINING + 512 };

static uint32_t random_value(uint32_t *state)
{
	*state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
	return *state;
}

static int16_t quantize(enum g711_law law, double sample)
{
	int16_t value = (int16_t)fmax(INT16_MIN, fmin(INT16_MAX, round(sample)));
	return law == G711_LAW_PCMU ? g711_decode_pcmu(g711_encode_pcmu(value)) :
		g711_decode_pcma(g711_encode_pcma(value));
}

static int compare_double(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return (x > y) - (x < y);
}

/* Independent integration: enumerate the encoder's integer transitions,
 * partition their union for all three inputs, and evaluate one point per
 * constant codec cell. This does not use the model's compiled geometry,
 * cached cells, transition formula, or likelihood implementation. */
static uint64_t independent_score(const struct v92_observation *model,
	const int32_t input[3], int16_t observed)
{
	double thresholds[256];
	size_t transitions = 0;
	int16_t last = quantize(model->law, INT16_MIN);
	for (int value = INT16_MIN + 1; value <= INT16_MAX; value++) {
		int16_t next = quantize(model->law, value);
		if (next != last) thresholds[transitions++] = value - .5;
		last = next;
	}
	assert(transitions <= 255);
	double lower = 0., upper = 0.;
	if (model->quantizer_gain > 0.) {
		struct g711_interval bin;
		assert(g711_quantization_interval(model->law,
			quantize(model->law, observed / model->quantizer_gain), &bin));
		lower = bin.lower == INT16_MIN ? -HUGE_VAL : (bin.lower - .5) * model->quantizer_gain;
		upper = bin.upper == INT16_MAX ? HUGE_VAL : (bin.upper + .5) * model->quantizer_gain;
		lower -= observed;
		upper -= observed;
	}
	double likelihood = 0.;
	for (unsigned i = 0; i < V92_OBSERVATION_INTERVALS; i++) {
		const struct v92_observation_interval *fit = &model->intervals[i];
		if (fit->weight == 0.) continue;
		double cuts[3 * 256 + 2] = { fit->low, fit->high };
		size_t count = 2;
		for (unsigned j = 0; j < 3; j++) {
			if (!input[j]) continue;
			for (size_t k = 0; k < transitions; k++) {
				double gain = thresholds[k] / input[j];
				if (gain > fit->low && gain < fit->high) cuts[count++] = gain;
			}
		}
		qsort(cuts, count, sizeof(*cuts), compare_double);
		double integral = 0.;
		for (size_t j = 1; j < count; j++) {
			if (cuts[j] == cuts[j - 1]) continue;
			double gain = sqrt(cuts[j - 1] * cuts[j]);
			double error = observed - fit->main * quantize(model->law, gain * input[0]) -
				fit->post * quantize(model->law, gain * input[1]) -
				fit->second * quantize(model->law, gain * input[2]);
			double probability;
			if (model->quantizer_gain == 0.) {
				probability = exp(-.5 * error * error / model->variance);
			} else {
				double a = (error + lower) / sqrt(2. * model->variance);
				double b = (error + upper) / sqrt(2. * model->variance);
				probability = a > 0. ? .5 * (erfc(a) - erfc(b)) :
					b < 0. ? .5 * (erfc(-b) - erfc(-a)) : .5 * (erf(b) - erf(a));
			}
			integral += probability * log1p((cuts[j] - cuts[j - 1]) / cuts[j - 1]);
		}
		likelihood += fit->weight * integral / log1p((fit->high - fit->low) / fit->low);
	}
	return (uint64_t)(-2e6 * log(fmin(1., fmax(1e-300, likelihood))));
}

static void check_geometry(struct v92_observation_cache *cache)
{
	const struct v92_observation *model = cache->model;
	for (size_t i = 0; i < model->count; i++) {
		for (int offset = -1; offset <= 1; offset++) {
			int64_t wide = (int64_t)model->first[i] + offset;
			if (wide < INT32_MIN || wide > INT32_MAX) continue;
			int32_t x = (int32_t)wide;
			size_t expected = 0;
			while (expected + 1 < model->count && model->first[expected + 1] <= x) expected++;
			struct v92_observation_point point = v92_observation_point(cache, x);
			assert(point.span == expected);
			struct v92_observation_range range = v92_observation_range(cache, x);
			assert(range.low == point.values.main_low);
			assert(range.high == point.values.main_high);
		}
	}
	assert(v92_observation_point(cache, INT32_MAX).span == model->count - 1);
}

static void check_failures(struct v92_observation *model, const int32_t *x,
	const int16_t *u, const int16_t *y)
{
	struct v92_observation before = *model;
	assert(!v92_observation_fit(model, model->law, x, u, y, TRAINING + 1));
	assert(!v92_observation_fit(model, model->law, x, u, y, 3));
	assert(!v92_observation_fit(model, (enum g711_law)-1, x, u, y, TRAINING));
	assert(!v92_observation_fit(model, model->law, NULL, u, y, TRAINING));
	assert(!v92_observation_fit(model, model->law, x, NULL, y, TRAINING));
	assert(!v92_observation_fit(model, model->law, x, u, NULL, TRAINING));
	assert(!v92_observation_fit(NULL, model->law, x, u, y, TRAINING));
	assert(!v92_observation_fit(model, model->law, x, y, y, TRAINING));
	assert(!v92_observation_fit(model, model->law, model->first, u, y, TRAINING));
	assert(!v92_observation_fit(model, model->law, x, (const int16_t *)model, y, TRAINING));
	assert(!v92_observation_fit(model, model->law, x, u, (const int16_t *)model, TRAINING));
	assert(!v92_observation_refit_bins(model, x, y, y, TRAINING + 1));
	assert(!v92_observation_refit_bins(model, model->first, y, y, TRAINING));
	assert(!v92_observation_refit_bins(model, x, (const int16_t *)model, y, TRAINING));
	assert(!v92_observation_refit_bins(model, x, y, (const int16_t *)model, TRAINING));
	assert(!v92_observation_refit_bins(model, NULL, y, y, TRAINING));
	int16_t invalid[TRAINING] = { 1 }; /* 1 is not a reconstructed codec level. */
	assert(!v92_observation_refit_bins(model, x, y, invalid, TRAINING));
	assert(memcmp(model, &before, sizeof(before)) == 0);
	assert(!v92_observation_is_valid(NULL));
	model->variance = NAN;
	assert(!v92_observation_is_valid(model));
	*model = before;
	model->intervals[0].weight = -1.;
	assert(!v92_observation_is_valid(model));
	*model = before;
	model->bucket[0] = UINT16_MAX;
	assert(!v92_observation_is_valid(model));
	*model = before;
	model->first[1] = model->first[0];
	assert(!v92_observation_is_valid(model));
	*model = before;
	model->quantizer_gain = 0.;
	assert(!v92_observation_refit_bins(model, x, y, y, TRAINING));
	assert(isnan(v92_observation_predict(model, x)));
	*model = before;
}

static void check_scores(const struct v92_observation *model,
	const int32_t *x, const int16_t *y)
{
	struct v92_observation_cache cache;
	memset(&cache, 0xa5, sizeof(cache));
	v92_observation_begin(&cache, model, .001);
	check_geometry(&cache);
	for (size_t i = TRAINING; i < SAMPLES; i++) {
		assert(v92_observation_symbol_level(&cache, y[i], y + i));
		struct v92_observation_point previous = v92_observation_point(&cache, x[i - 1]);
		struct v92_observation_point second = v92_observation_point(&cache, x[i - 2]);
		int32_t candidates[] = { x[i], -x[i], 0, INT32_MIN, INT32_MAX };
		for (unsigned k = 0; k < sizeof(candidates) / sizeof(candidates[0]); k++) {
			uint64_t exact = v92_observation_score(&cache, candidates[k], &previous, &second, UINT64_MAX);
			uint64_t budgets[] = { 0, exact / 2, exact, exact + 1, UINT64_MAX };
			for (unsigned j = 0; j < sizeof(budgets) / sizeof(budgets[0]); j++) {
				uint64_t value = v92_observation_score(&cache, candidates[k], &previous, &second, budgets[j]);
				assert(value <= exact);
				assert(value == exact || value >= budgets[j]);
			}
			assert(v92_observation_score(&cache, candidates[k], &previous, &second, UINT64_MAX) == exact);
		}
		/* Invalidate every score through a fresh cache and compare the result,
		 * including across the 8-bit per-symbol generation wrap. */
		uint64_t score = v92_observation_score(&cache, x[i], &previous, &second, UINT64_MAX);
		struct v92_observation_cache fresh;
		v92_observation_begin(&fresh, model, .001);
		assert(v92_observation_symbol(&fresh, y[i]));
		assert(v92_observation_score(&fresh, x[i], &previous, &second, UINT64_MAX) == score);
	}
	struct v92_observation point_model = *model;
	point_model.quantizer_gain = 0.;
	for (unsigned bins = 0; bins < 2; bins++) {
		const struct v92_observation *reference = bins ? model : &point_model;
		v92_observation_begin(&cache, reference, 0.);
		for (unsigned i = TRAINING; i < TRAINING + 8; i++) {
			int32_t input[] = { x[i], x[i - 1], x[i - 2] };
			struct v92_observation_point b = v92_observation_point(&cache, input[1]);
			struct v92_observation_point c = v92_observation_point(&cache, input[2]);
			assert(v92_observation_symbol(&cache, y[i]));
			uint64_t actual = v92_observation_score(&cache, input[0], &b, &c, UINT64_MAX);
			uint64_t expected = independent_score(reference, input, y[i]);
			assert(actual <= expected + 1 && expected <= actual + 1);
		}
	}
	struct v92_observation_cache saved = cache;
	int16_t invalid = 1;
	assert(!v92_observation_symbol_level(&cache, 42, &invalid));
	assert(memcmp(&cache, &saved, sizeof(cache)) == 0);
	/* The known input rounding belongs before the first codec. */
	struct v92_observation rounded = *model;
	rounded.input_coefficient = 24576;
	v92_observation_begin(&cache, &rounded, 0.);
	int32_t input[] = { -19234, 22857, 5377 }, transformed[3];
	for (unsigned i = 0; i < 3; i++) {
		transformed[i] = round_shift_s64_to_s32_saturating((int64_t)input[i] * 24576, 15u);
		assert(v92_observation_point(&cache, input[i]).x == transformed[i]);
	}
	assert(v92_observation_predict(&rounded, input) == v92_observation_predict(model, transformed));
	cache.relative_noise = NAN;
	assert(!v92_observation_symbol(&cache, 0));
	cache.relative_noise = -1.;
	assert(!v92_observation_symbol(&cache, 0));
	rounded.quantizer_gain = DBL_TRUE_MIN;
	assert(v92_observation_is_valid(&rounded));
	v92_observation_begin(&cache, &rounded, 0.);
	assert(v92_observation_symbol(&cache, 0));
}

static void check_channel(enum g711_law law, double gain)
{
	int32_t x[SAMPLES];
	int16_t q[SAMPLES], y[SAMPLES], u[TRAINING] = { 0 };
	uint32_t state = UINT32_C(0x182dab72);
	for (unsigned i = 0; i < SAMPLES; i++) {
		x[i] = (int32_t)round(((int)(random_value(&state) % 40001) - 20000) / gain);
		q[i] = quantize(law, gain * x[i]);
		double sample = .63 * q[i] + (i ? .07 * q[i - 1] : 0.) - (i > 1 ? .02 * q[i - 2] : 0.);
		y[i] = quantize(law, sample);
	}
	struct v92_observation model;
	assert(v92_observation_fit(&model, law, x, u, y, TRAINING));
	assert(v92_observation_is_valid(&model));
	model.quantizer_gain = 1.;
	assert(v92_observation_refit_bins(&model, x, y, y, TRAINING));
	assert(v92_observation_is_valid(&model));
	struct v92_observation before = model;
	double error = 0.;
	for (size_t i = TRAINING; i < SAMPLES; i++) {
		int32_t input[] = { x[i], x[i - 1], x[i - 2] };
		double residual = v92_observation_predict(&model, input) - y[i];
		error += residual * residual;
	}
	double rms = sqrt(error / (SAMPLES - TRAINING));
	assert(isfinite(rms) && rms < 100.);
	check_scores(&model, x, y);
	assert(memcmp(&model, &before, sizeof(model)) == 0);
	check_failures(&model, x, u, y);
	printf("V.92 observation law=%u gain=%.9g heldout_rms=%.6f\n", law, gain, rms);
}

int main(void)
{
	const double gains[] = { .00002, .02, .125, 4., 16.39453125, 21.859375, 512. };
	for (unsigned law = G711_LAW_PCMU; law <= G711_LAW_PCMA; law++)
		for (unsigned i = 0; i < sizeof(gains) / sizeof(gains[0]); i++)
			check_channel(law, gains[i]);
	struct v92_observation model, before;
	memset(&model, 0x5a, sizeof(model));
	before = model;
	int32_t zero[TRAINING] = { 0 };
	int16_t y[TRAINING] = { 0 };
	assert(!v92_observation_fit(&model, G711_LAW_PCMU, zero, y, y, TRAINING));
	assert(memcmp(&model, &before, sizeof(model)) == 0);
	puts("V.92 observation checks passed");
	return 0;
}
