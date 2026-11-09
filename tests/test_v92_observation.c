// SPDX-License-Identifier: BSD-3-Clause
#include "v92_observation.h"
#include "v92_observation_fit.h"
#include "integer_math.h"

#include <assert.h>
#include <fenv.h>
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

/* Independent complete sum of final-codec cell probabilities. */
static double independent_mean(enum g711_law law, double mean, double sigma)
{
	double sum = 0.;
	for (unsigned code = 0; code < 256; ++code) {
		int16_t level = law == G711_LAW_PCMU ? g711_decode_pcmu(code) : g711_decode_pcma(code);
		if (law == G711_LAW_PCMU && code == 127) continue; /* Shared zero bin. */
		struct g711_interval bin;
		assert(g711_quantization_interval(law, level, &bin));
		double a = bin.lower == INT16_MIN ? -HUGE_VAL : (bin.lower - .5 - mean) / (sqrt(2.) * sigma);
		double b = bin.upper == INT16_MAX ? HUGE_VAL : (bin.upper + .5 - mean) / (sqrt(2.) * sigma);
		double probability = a > 0. ? .5 * (erfc(a) - erfc(b)) :
			b < 0. ? .5 * (erfc(-b) - erfc(-a)) : .5 * (erf(b) - erf(a));
		sum += level * probability;
	}
	return sum;
}

/* Enumerate original wire code cells, then apply the wire operation. This
 * is independent of the predictor's adjacent-cell tail walk and includes
 * both signed zero characters. */
static double independent_mean_lsb(enum g711_law law, double mean, double sigma,
	enum g711_lsb_mode mode)
{
	double sum = 0.;
	for (unsigned code = 0; code < 256; ++code) {
		struct g711_interval bin;
		assert(g711_octet_interval(law, code, &bin));
		double a = bin.lower == INT16_MIN ? -HUGE_VAL : (bin.lower - .5 - mean) / (sqrt(2.) * sigma);
		double b = bin.upper == INT16_MAX ? HUGE_VAL : (bin.upper + .5 - mean) / (sqrt(2.) * sigma);
		double probability = a > 0. ? .5 * (erfc(a) - erfc(b)) :
			b < 0. ? .5 * (erfc(-b) - erfc(-a)) : .5 * (erf(b) - erf(a));
		unsigned mapped = mode == G711_LSB_UNCHANGED ? code :
			(code & 254u) | (mode == G711_LSB_ONE);
		sum += g711_decode(law, mapped) * probability;
	}
	return sum;
}

/* Independent integration: enumerate the encoder's integer transitions,
 * partition their union for all active inputs, and evaluate one point per
 * constant codec cell. This does not use the model's compiled geometry,
 * cached cells, transition formula, or likelihood implementation. */
static uint64_t independent_score_phase(const struct v92_observation *model,
	const int32_t *input, int16_t observed, int16_t level, double offset,
	unsigned phase, double *prediction)
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
	struct g711_interval bin;
	enum g711_lsb_mode mode = model->lsb[phase];
	/* Independent preimage of the observed reconstruction. */
	int low = INT16_MAX, high = INT16_MIN;
	if (mode == G711_LSB_UNCHANGED) {
		assert(g711_quantization_interval(model->law, level, &bin));
		low = bin.lower; high = bin.upper;
	}
	for (int value = INT16_MIN; mode != G711_LSB_UNCHANGED && value <= INT16_MAX; ++value) {
		unsigned code = g711_encode(model->law, value);
		unsigned mapped = mode == G711_LSB_UNCHANGED ? code :
			(code & 254u) | (mode == G711_LSB_ONE);
		if (g711_decode(model->law, mapped) == level) {
			if (value < low) low = value;
			if (value > high) high = value;
		}
	}
	assert(model->quantizer_gain > 0. && low <= high);
	bin = (struct g711_interval){ low, high };
	double lower = bin.lower == INT16_MIN ? -HUGE_VAL : (bin.lower - .5) * model->quantizer_gain;
	double upper = bin.upper == INT16_MAX ? HUGE_VAL : (bin.upper + .5) * model->quantizer_gain;
	lower += offset - observed;
	upper += offset - observed;
	double likelihood = 0.;
	*prediction = offset;
	for (unsigned i = 0; i < V92_OBSERVATION_INTERVALS; i++) {
		const struct v92_observation_interval *fit = &model->intervals[i];
		if (fit->weight == 0.) continue;
		double cuts[V92_OBSERVATION_TAPS * 256 + 2] = { fit->low, fit->high };
		size_t count = 2;
		for (unsigned j = 0; j < model->taps; j++) {
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
			double error = observed;
			for (unsigned k = 0; k < model->taps; k++)
				error -= fit->coefficient[k] * quantize(model->law, gain * input[k]);
			*prediction += fit->weight * model->quantizer_gain *
				independent_mean_lsb(model->law, (observed - error - offset) / model->quantizer_gain,
					sqrt(model->variance) / model->quantizer_gain, mode) *
				log1p((cuts[j] - cuts[j - 1]) / cuts[j - 1]) / log1p((fit->high - fit->low) / fit->low);
			double probability;
			if (lower == upper) {
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

static uint64_t independent_score(const struct v92_observation *model,
	const int32_t *input, int16_t observed, int16_t level, double offset,
	double *prediction)
{
	return independent_score_phase(model, input, observed, level, offset, 0, prediction);
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
			assert(range.low == point.values.low[0]);
			assert(range.high == point.values.high[0]);
		}
	}
	/* Revisit coordinates after more distinct lookups than the bounded
	 * memo can hold. Replacements must preserve exact geometry. */
	for (size_t i = model->count; i-- > 0;) {
		int32_t x = model->first[i];
		struct v92_observation_range range = v92_observation_range(cache, x);
		struct v92_observation_point point = v92_observation_point(cache, x);
		assert(point.span == i && range.low == point.values.low[0] &&
			range.high == point.values.high[0]);
	}
	assert(v92_observation_point(cache, INT32_MAX).span == model->count - 1);
}

static void check_failures(struct v92_observation *model, const int32_t *x,
	const int16_t *u, const int16_t *y)
{
	struct v92_observation before = *model;
	const struct v92_observation_training good = {
		.input = x, .linear = u, .sample = y, .levels = y,
		.count = TRAINING, .taps = model->taps, .host_gain = model->quantizer_gain, .law = model->law,
	};
	enum g711_lsb_mode invalid_lsb[V92_OBSERVATION_PHASES] = { [5] = (enum g711_lsb_mode)3 };
	enum g711_lsb_mode no_lsb[V92_OBSERVATION_PHASES] = { 0 };
	int16_t invalid[TRAINING] = { 1 }; /* Not a reconstructed codec level. */
	assert(!v92_observation_fit(NULL, &good));
	assert(!v92_observation_fit(model, NULL));
	assert(!v92_observation_fit(model, (const struct v92_observation_training *)model));
	double offsets[TRAINING] = { 0 };
	for (unsigned fault = 0; fault < 28; fault++) {
		struct v92_observation_training bad = good;
		switch (fault) {
		case 0: bad.count = TRAINING + 1; break;
		case 1: bad.count = 3; break;
		case 2: bad.law = (enum g711_law)-1; break;
		case 3: bad.input = NULL; break;
		case 4: bad.linear = NULL; break;
		case 5: bad.sample = NULL; break;
		case 6: bad.linear = y; break;
		case 7: bad.input = model->first; break;
		case 8: bad.linear = (const int16_t *)model; break;
		case 9: bad.sample = (const int16_t *)model; break;
		case 10: bad.levels = (const int16_t *)model; break;
		case 11: bad.levels = invalid; break;
		case 12: bad.host_gain = 0.; break;
		case 13: bad.host_gain = -1.; break;
		case 14: bad.host_gain = NAN; break;
		case 15: bad.host_gain = INFINITY; break;
		case 16: bad.offsets = (const double *)model; break;
		case 17: bad.offsets = offsets; bad.levels = NULL; break;
		case 18: bad.offsets = offsets; offsets[TRAINING - 1] = NAN; break;
		case 19: bad.offsets = offsets; offsets[TRAINING - 1] = INFINITY; break;
		case 20: bad.offsets = offsets; offsets[TRAINING - 1] = -INFINITY; break;
		case 21: bad.taps = 0; break;
		case 22: bad.taps = 2; break;
		case 23: bad.taps = V92_OBSERVATION_TAPS + 1; break;
		case 24: bad.count = bad.taps; break;
		case 25: bad.lsb = (const enum g711_lsb_mode *)model; break;
		case 26: bad.lsb = invalid_lsb; break;
		case 27: bad.lsb = no_lsb; bad.levels = NULL; break;
		}
		assert(!v92_observation_fit(model, &bad));
		assert(memcmp(model, &before, sizeof(before)) == 0);
	}
	assert(!v92_observation_is_valid(NULL));
	model->taps = 2;
	assert(!v92_observation_is_valid(model));
	*model = before;
	model->taps = V92_OBSERVATION_TAPS + 1;
	assert(!v92_observation_is_valid(model));
	*model = before;
	model->intervals[0].coefficient[3] = NAN;
	assert(!v92_observation_is_valid(model));
	*model = before;
	if (model->taps == 3) {
		model->intervals[0].coefficient[3] = .01;
		assert(!v92_observation_is_valid(model));
		*model = before;
	}
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
	assert(!v92_observation_is_valid(model));
	assert(isnan(v92_observation_predict(model, x, 0.)));
	*model = before;
}

/* Integrate a rescaled Gaussian tail without erfc or underflow. With t=a*s,
 * the integrand is exp(-t-t*t/(2*a*a)); truncation at 60 loses < exp(-60).
 * Simpson integration is independent of the production likelihood code. */
static double tail_score(double a, double b)
{
	assert(a > 20. && b > a);
	const unsigned steps = 4096;
	double end = fmin(60., a * (b - a)), step = end / steps, integral = 0.;
	for (unsigned i = 0; i <= steps; i++) {
		double t = i * step;
		unsigned weight = i == 0 || i == steps ? 1 : i % 2 ? 4 : 2;
		integral += weight * exp(-t - .5 * (t / a) * (t / a));
	}
	integral *= step / 3.;
	return -2e6 * (-.5 * a * a - log(a) - .5 * log(2. * acos(-1.)) + log(integral));
}

static void check_tail_scores(const struct v92_observation *fitted)
{
	struct v92_observation model = *fitted;
	model.variance = 256.;
	model.quantizer_gain = 1.;
	for (unsigned i = 0; i < V92_OBSERVATION_INTERVALS; i++) {
		model.intervals[i].coefficient[0] = 1.;
		model.intervals[i].coefficient[1] = model.intervals[i].coefficient[2] = 0.;
		model.intervals[i].weight = i == 0 ? 1. : 0.;
	}
	assert(v92_observation_is_valid(&model));
	struct v92_observation_cache cache;
	v92_observation_begin(&cache, &model, 0.);
	struct v92_observation_point zero = v92_observation_point(&cache, 0);
	double mean = quantize(model.law, 0);
	const int magnitudes[] = { 512, 1024, 2048, 4096, 32767 };
	for (int sign = -1; sign <= 1; sign += 2) {
		uint64_t previous = 0;
		for (unsigned k = 0; k < sizeof(magnitudes) / sizeof(*magnitudes); k++) {
			int magnitude = magnitudes[k];
			int16_t observed = quantize(model.law, sign * magnitude);
			struct g711_interval bin;
			assert(g711_quantization_interval(model.law, observed, &bin));
			double a = bin.lower == INT16_MIN ? -HUGE_VAL : (bin.lower - .5 - mean) / 16.;
			double b = bin.upper == INT16_MAX ? HUGE_VAL : (bin.upper + .5 - mean) / 16.;
			if (sign < 0) { double swap = a; a = -b; b = -swap; }
			double expected = tail_score(a, b);
			assert(v92_observation_symbol_level(&cache, observed, &observed, 0.));
			uint64_t actual = v92_observation_score(&cache, 0, (struct v92_observation_point[]){ zero, zero }, UINT64_MAX);
			assert(fabs((double)actual - expected) <= 1.);
			assert(actual > previous);
			if (magnitude >= 1024) assert(actual > (uint64_t)(-2e6 * log(1e-300)));
			uint64_t budgets[] = { actual / 2, actual, actual + 1, UINT64_MAX };
			for (unsigned j = 0; j < sizeof(budgets) / sizeof(*budgets); j++) {
				uint64_t bounded = v92_observation_score(&cache, 0, (struct v92_observation_point[]){ zero, zero }, budgets[j]);
				assert(bounded <= actual && (bounded == actual || bounded >= budgets[j]));
			}
			previous = actual;
		}
	}
	/* The metric saturates at the integer representation limit, not at a
	 * probability floor. Exercise both finite and overflowing squared errors. */
	const double scales[] = { 1e8, 1e300 };
	for (unsigned i = 0; i < sizeof(scales) / sizeof(*scales); i++) {
		model.intervals[0].coefficient[0] = scales[i];
		assert(v92_observation_is_valid(&model));
		v92_observation_begin(&cache, &model, 0.);
		zero = v92_observation_point(&cache, 0);
		int16_t observed = quantize(model.law, 0);
		assert(v92_observation_symbol_level(&cache, observed, &observed, 0.));
		assert(v92_observation_score(&cache, INT32_MAX, (struct v92_observation_point[]){ zero, zero }, UINT64_MAX) == UINT64_MAX);
		assert(v92_observation_score(&cache, INT32_MAX, (struct v92_observation_point[]){ zero, zero }, 1) == UINT64_MAX);
	}
}

/* Move a real codec cell across normal and unrepresentable Gaussian tails.
 * Cover both signs and widths on either side of the negligible-tail bound;
 * directed rounding must still account for the subtracted tail. */
static void check_tail_cells(const struct v92_observation *fitted)
{
	const int modes[] = { FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO };
	const double starts[] = { .125, 1., 4., 12., 15., 26., 28., 64., 10000. };
	const double gaps[] = { 39.5, 40., 40.5, 80. };
	int saved = fegetround();
	assert(saved != -1);
	struct v92_observation model = *fitted;
	model.quantizer_gain = 4.;
	for (unsigned i = 0; i < V92_OBSERVATION_INTERVALS; i++) {
		model.intervals[i].coefficient[0] = 1.;
		model.intervals[i].coefficient[1] = model.intervals[i].coefficient[2] = 0.;
		model.intervals[i].weight = i == 0 ? 1. : 0.;
	}
	int16_t level = quantize(model.law, 0);
	struct g711_interval bin;
	assert(g711_quantization_interval(model.law, level, &bin));
	for (unsigned mode = 0; mode < sizeof(modes) / sizeof(modes[0]); mode++) {
		assert(fesetround(modes[mode]) == 0);
		for (unsigned i = 0; i < sizeof(starts) / sizeof(starts[0]); i++)
			for (unsigned j = 0; j < sizeof(gaps) / sizeof(gaps[0]); j++)
				for (unsigned reflected = 0; reflected < 2; reflected++) {
					double a = starts[i], b = sqrt(a * a + gaps[j]);
					double divisor = (bin.upper + 1. - bin.lower) * model.quantizer_gain / (b - a);
					model.variance = .5 * divisor * divisor;
					assert(v92_observation_is_valid(&model));
					struct v92_observation_cache cache;
					v92_observation_begin(&cache, &model, 0.);
					struct v92_observation_point zero = v92_observation_point(&cache, 0);
					double offset = level + (reflected ? -b : a) * divisor - (bin.lower - .5) * model.quantizer_gain;
					assert(v92_observation_symbol_level(&cache, 0, &level, offset));
					a = (cache.lower - level) / sqrt(model.variance);
					b = (cache.upper - level) / sqrt(model.variance);
					if (reflected) { double swap = a; a = -b; b = -swap; }
					/* Independently integrate deep tails; normal masses use the complete
					 * erfc subtraction, including the term the optimized scorer may omit. */
					double expected = a > 20. ? tail_score(a, b) :
						-2e6 * log(.5 * (erfc(a / sqrt(2.)) - erfc(b / sqrt(2.))));
					uint64_t actual = v92_observation_score(&cache, 0,
						(struct v92_observation_point[]){ zero, zero }, UINT64_MAX);
					assert(fabs((double)actual - expected) <= 2.);
				}
	}
	assert(fesetround(saved) == 0);
}

/* Constant inputs remove the gain integral: every fitted interval has the
 * same Gaussian mass. Independently retain the old log-domain mixture at
 * integer-score boundaries, where different summation orders can round to
 * opposite sides of a truncation. Do not floor the reference probability. */
static bool constant_input_score(const struct v92_observation_cache *cache,
	int16_t mean, uint64_t *score)
{
	double a = (cache->lower - mean) / cache->divisor;
	double b = (cache->upper - mean) / cache->divisor;
	double probability = a > 0. ? .5 * (erfc(a) - erfc(b)) :
		b < 0. ? .5 * (erfc(-b) - erfc(-a)) :
		.5 * ((b >= 6. ? 1. : erf(b)) - (a <= -6. ? -1. : erf(a)));
	if (probability < DBL_MIN) return false;
	double likelihood = -HUGE_VAL;
	for (unsigned i = 0; i < V92_OBSERVATION_INTERVALS; i++) {
		double term = log(cache->model->intervals[i].weight) + log(probability);
		likelihood = i == 0 ? term : fmax(likelihood, term) +
			log1p(exp(fmin(likelihood, term) - fmax(likelihood, term)));
	}
	double value = -2e6 * likelihood;
	*score = value <= 0. ? 0 : (uint64_t)value;
	return true;
}

static void check_integer_scores(const struct v92_observation *fitted)
{
	const int modes[] = { FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO };
	const uint64_t targets[] = { 1, 2, 7, 31, 1024, 1000000, 100000000, 900000000 };
	int saved = fegetround();
	assert(saved != -1 && fesetround(FE_TONEAREST) == 0);
	struct v92_observation model = *fitted;
	model.quantizer_gain = 4.;
	model.variance = 1.;
	for (unsigned i = 0; i < V92_OBSERVATION_INTERVALS; i++) {
		memset(model.intervals[i].coefficient, 0, sizeof(model.intervals[i].coefficient));
		model.intervals[i].coefficient[0] = 1.;
		model.intervals[i].weight = (i + 1.) / 36.;
	}
	assert(v92_observation_is_valid(&model));
	int16_t level = quantize(model.law, 0);
	struct g711_interval bin;
	assert(g711_quantization_interval(model.law, level, &bin));
	double center = level - .5 * (bin.lower + bin.upper) * model.quantizer_gain;
	struct v92_observation_cache cache;
	for (unsigned mode = 0; mode < sizeof(modes) / sizeof(*modes); mode++) {
		assert(fesetround(modes[mode]) == 0);
		for (unsigned t = 0; t < sizeof(targets) / sizeof(*targets); t++) {
			double low = 0., high = 100000.;
			/* Locate a score transition, then test adjacent representable offsets. */
			for (unsigned i = 0; i < 60; i++) {
				double mid = low + .5 * (high - low);
				v92_observation_begin(&cache, &model, 0.);
				assert(v92_observation_symbol_level(&cache, 0, &level, center + mid));
				uint64_t score;
				if (!constant_input_score(&cache, level, &score) || score >= targets[t]) high = mid;
				else low = mid;
			}
			for (int neighbor = -4; neighbor <= 4; neighbor++) {
				double offset = high;
				for (int i = 0; i < abs(neighbor); i++)
					offset = nextafter(offset, neighbor < 0 ? -INFINITY : INFINITY);
				v92_observation_begin(&cache, &model, 0.);
				assert(v92_observation_symbol_level(&cache, 0, &level, center + offset));
				struct v92_observation_point zero = v92_observation_point(&cache, 0);
				uint64_t expected;
				assert(constant_input_score(&cache, level, &expected));
				uint64_t actual = v92_observation_score(&cache, 0,
					(struct v92_observation_point[]){ zero, zero }, UINT64_MAX);
				assert(actual == expected);
			}
		}
	}
	assert(fesetround(saved) == 0);
}

/* Reuse only the physical past across different current points and observed
 * codec cells and noise estimates. Compare with a fresh general evaluation. */
static void check_history_scores(const struct v92_observation *model,
	const int32_t *x, const int16_t *y)
{
	struct history_workspace { struct v92_observation_cache reference, actual; };
	struct history_workspace *work = malloc(sizeof(*work));
	assert(work);
	int saved = fegetround();
	const int modes[] = { FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO };
	for (unsigned m = 0; m < sizeof(modes) / sizeof(*modes); m++) {
		assert(fesetround(modes[m]) == 0);
		v92_observation_begin(&work->reference, model, .00001);
		v92_observation_begin(&work->actual, model, .00001);
		for (size_t i = TRAINING; i < SAMPLES; i += 17) {
			struct v92_observation_point previous[V92_OBSERVATION_TAPS - 1];
			for (unsigned j = 1; j < model->taps; j++)
				previous[j - 1] = v92_observation_point(&work->actual, x[i - j]);
			struct v92_observation_history history;
			v92_observation_history_reset(&history);
			const int32_t candidates[] = { x[i], x[i] ^ 1, x[i] / 2, 0, INT32_MIN, INT32_MAX };
			for (unsigned cell = 0; cell < 3; cell++) {
				work->reference.relative_noise = work->actual.relative_noise = cell * .00001;
				double offset = ((int)cell - 1) * .25;
				assert(v92_observation_symbol_level(&work->reference, y[i], y + i, offset));
				assert(v92_observation_symbol_level(&work->actual, y[i], y + i, offset));
				for (unsigned k = 0; k < sizeof(candidates) / sizeof(*candidates); k++) {
					uint64_t expected = v92_observation_score(&work->reference, candidates[k], previous, UINT64_MAX);
					uint64_t actual = v92_observation_score_history(&work->actual, candidates[k], previous, UINT64_MAX, &history);
					assert(actual == expected);
					const uint64_t budgets[] = { expected / 2, expected == UINT64_MAX ? UINT64_MAX : expected + 1 };
					for (unsigned j = 0; j < 2; j++)
						assert(v92_observation_score_history(&work->actual, candidates[k], previous, budgets[j], &history) ==
						 v92_observation_score(&work->reference, candidates[k], previous, budgets[j]));
				}
			}
		}
	}
	assert(fesetround(saved) == 0);
	free(work);
}

/* Mixture taps must share their gain interval. Opposing delayed taps
 * cancel within each component; combining their independent extremes would
 * admit unrelated gains and force an unnecessary full likelihood. */
static void check_coupled_bounds(const struct v92_observation *trained, bool disjoint)
{
	struct v92_observation model = *trained;
	if (disjoint) model.variance = 4.;
	for (unsigned i = 0; i < V92_OBSERVATION_INTERVALS; i++) {
		/* Valid models admit a small excess in the sum of weights. */
		model.intervals[i].weight = 1. / V92_OBSERVATION_INTERVALS + DBL_EPSILON;
		model.intervals[i].coefficient[0] = 1.;
		model.intervals[i].coefficient[1] = i & 1 ? .8 : -.8;
		model.intervals[i].coefficient[2] = disjoint ? 0. : -model.intervals[i].coefficient[1];
		model.intervals[i].coefficient[3] = 0.;
	}
	assert(v92_observation_is_valid(&model));
	struct v92_observation_cache *cache = malloc(2 * sizeof(*cache));
	assert(cache);
	const int modes[] = { FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO };
	const int32_t points[] = { 1, 33, 4096, -4096, INT32_MIN, INT32_MAX };
	int saved = fegetround(); unsigned pruned = 0;
	for (unsigned mode = 0; mode < sizeof(modes) / sizeof(*modes); mode++) {
		assert(fesetround(modes[mode]) == 0);
		v92_observation_begin(cache, &model, .00001);
		v92_observation_begin(cache + 1, &model, .00001);
		for (unsigned p = 0; p < sizeof(points) / sizeof(*points); p++) {
			struct v92_observation_point history[V92_OBSERVATION_TAPS - 1];
			for (unsigned j = 1; j < model.taps; j++)
				history[j - 1] = v92_observation_point(cache, disjoint ? 4096 : points[p]);
			struct v92_observation_history fixed;
			memset(&fixed, 0xa5, sizeof(fixed)); v92_observation_history_reset(&fixed);
			for (unsigned cell = 0; cell < 3; cell++) {
				int16_t observed = cell == 0 ? 0 : cell == 1 ? 3000 : -3000;
				assert(v92_observation_symbol(cache, observed));
				assert(v92_observation_symbol(cache + 1, observed));
				uint64_t exact = v92_observation_score(cache + 1, points[p], history, UINT64_MAX);
				const uint64_t budgets[] = { 0, 1, exact / 2, exact, exact == UINT64_MAX ? exact : exact + 1, UINT64_MAX };
				for (unsigned b = 0; b < sizeof(budgets) / sizeof(*budgets); b++) {
					uint64_t actual = v92_observation_score_history(cache, points[p], history, budgets[b], &fixed);
					assert(actual <= exact);
					assert(actual == exact || actual >= budgets[b]);
					assert(actual == v92_observation_score(cache, points[p], history, budgets[b]));
					/* Separated gain hypotheses straddle the observed cell,
					 * but neither component can reach it. Their convex hull
					 * must not force exact tail integration. */
					if (disjoint && cell == 0 && points[p] == 1 && budgets[b] == 1)
						assert(actual < exact);
					pruned += budgets[b] == 1 && cell == 0 &&
						(points[p] == 4096 || points[p] == -4096) && actual != exact;
				}
			}
		}
	}
	assert(pruned > 0);
	assert(fesetround(saved) == 0);
	free(cache);
}

/* A validated model may still have coefficients whose weighted codec sums
 * exceed binary64. Under toward-zero rounding overflow can remain finite.
 * Such geometry must decline pruning rather than reject an exact candidate.
 * Changing coefficients does not change the fitted gain-cell partition. */
static void check_extreme_bounds(const struct v92_observation *trained)
{
	struct v92_observation model = *trained;
	struct v92_observation_cache *cache = malloc(2 * sizeof(*cache));
	assert(cache);
	const int modes[] = { FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO };
	const double scales[] = {
		DBL_MAX / (65536. * V92_OBSERVATION_TAPS),
		nextafter(DBL_MAX / (65536. * V92_OBSERVATION_TAPS), INFINITY), 1e308,
	};
	int saved = fegetround();
	model.variance = 1e9;
	model.quantizer_gain = .25;
	model.input_coefficient = 32767;
	for (unsigned mode = 0; mode < sizeof(modes) / sizeof(*modes); mode++) {
		assert(fesetround(modes[mode]) == 0);
		for (unsigned scale = 0; scale < sizeof(scales) / sizeof(*scales); scale++) {
			for (unsigned i = 0; i < V92_OBSERVATION_INTERVALS; i++) {
				model.intervals[i].coefficient[0] = scales[scale];
				model.intervals[i].coefficient[1] = -scales[scale];
				model.intervals[i].coefficient[2] = (i & 1 ? .25 : -.25) * scales[scale];
				model.intervals[i].coefficient[3] = 0.;
			}
			assert(v92_observation_is_valid(&model));
			v92_observation_begin(cache, &model, 0.);
			v92_observation_begin(cache + 1, &model, 0.);
			const int32_t input[] = { 33, INT32_MIN, 33, 33 };
			struct v92_observation_point previous[V92_OBSERVATION_TAPS - 1];
			for (unsigned j = 1; j < model.taps; j++)
				previous[j - 1] = v92_observation_point(cache, input[j]);
			int16_t level = quantize(model.law, 0);
			assert(v92_observation_symbol_level(cache + 1, 0, &level, -.25));
			uint64_t exact = v92_observation_score(cache + 1, input[0], previous, UINT64_MAX);
			const uint64_t budgets[] = { 1, exact / 2, exact, exact == UINT64_MAX ? exact : exact + 1, UINT64_MAX };
			for (unsigned b = 0; b < sizeof(budgets) / sizeof(*budgets); b++) {
				assert(v92_observation_symbol_level(cache, 0, &level, -.25));
				uint64_t actual = v92_observation_score(cache, input[0], previous, budgets[b]);
				assert(actual <= exact && (actual == exact || actual >= budgets[b]));
				assert(v92_observation_score(cache, input[0], previous, UINT64_MAX) == exact);
			}
		}
	}
	assert(fesetround(saved) == 0);
	free(cache);
}

static void check_scores(const struct v92_observation *model,
	const int32_t *x, const int16_t *y)
{
	struct v92_observation_cache cache;
	memset(&cache, 0xa5, sizeof(cache));
	v92_observation_begin(&cache, model, .001);
	check_geometry(&cache);
	for (size_t i = TRAINING; i < SAMPLES; i++) {
		assert(v92_observation_symbol_level(&cache, y[i], y + i, 0.));
		struct v92_observation_point history[V92_OBSERVATION_TAPS - 1];
		for (unsigned j = 1; j < model->taps; j++)
			history[j - 1] = v92_observation_point(&cache, x[i - j]);
		int32_t candidates[] = { x[i], -x[i], 0, INT32_MIN, INT32_MAX };
		for (unsigned k = 0; k < sizeof(candidates) / sizeof(candidates[0]); k++) {
			uint64_t exact = v92_observation_score(&cache, candidates[k], history, UINT64_MAX);
			uint64_t budgets[] = { 0, exact / 2, exact, exact + 1, UINT64_MAX };
			for (unsigned j = 0; j < sizeof(budgets) / sizeof(budgets[0]); j++) {
				uint64_t value = v92_observation_score(&cache, candidates[k], history, budgets[j]);
				assert(value <= exact);
				assert(value == exact || value >= budgets[j]);
			}
			assert(v92_observation_score(&cache, candidates[k], history, UINT64_MAX) == exact);
		}
		/* Invalidate every score through a fresh cache and compare the result,
		 * including across the 8-bit per-symbol generation wrap. */
		uint64_t score = v92_observation_score(&cache, x[i], history, UINT64_MAX);
		struct v92_observation_cache fresh;
		v92_observation_begin(&fresh, model, .001);
		assert(v92_observation_symbol(&fresh, y[i]));
		assert(v92_observation_score(&fresh, x[i], history, UINT64_MAX) == score);
		/* A changed oldest symbol must not reuse the preceding history's
		 * cached score, including when only the new fourth tap changes. */
		unsigned oldest = model->taps - 2;
		struct v92_observation_point saved = history[oldest];
		history[oldest] = v92_observation_point(&cache, -history[oldest].x);
		uint64_t changed = v92_observation_score(&cache, x[i], history, UINT64_MAX);
		v92_observation_begin(&fresh, model, .001);
		assert(v92_observation_symbol(&fresh, y[i]));
		assert(v92_observation_score(&fresh, x[i], history, UINT64_MAX) == changed);
		history[oldest] = saved;
		assert(v92_observation_score(&cache, x[i], history, UINT64_MAX) == score);
	}
	v92_observation_begin(&cache, model, 0.);
	for (unsigned i = TRAINING; i < TRAINING + 8; i++) {
		int32_t input[] = { x[i], x[i - 1], x[i - 2], x[i - 3] };
		struct v92_observation_point history[V92_OBSERVATION_TAPS - 1];
		for (unsigned j = 1; j < model->taps; j++)
			history[j - 1] = v92_observation_point(&cache, input[j]);
		assert(v92_observation_symbol(&cache, y[i]));
		uint64_t actual = v92_observation_score(&cache, input[0], history, UINT64_MAX);
		double prediction;
		uint64_t expected = independent_score(model, input, y[i], y[i], 0., &prediction);
		assert(actual <= expected + 1 && expected <= actual + 1);
		assert(fabs(v92_observation_predict(model, input, 0.) - prediction) < 1e-7);
	}
	struct v92_observation_cache saved = cache;
	int16_t invalid = 1;
	assert(!v92_observation_symbol_level(&cache, 42, &invalid, 0.));
	assert(memcmp(&cache, &saved, sizeof(cache)) == 0);
	/* The known input rounding belongs before the first codec. */
	struct v92_observation rounded = *model;
	rounded.input_coefficient = 24576;
	v92_observation_begin(&cache, &rounded, 0.);
	int32_t input[] = { -19234, 22857, 5377, -831 }, transformed[V92_OBSERVATION_TAPS];
	for (unsigned i = 0; i < model->taps; i++) {
		transformed[i] = round_shift_s64_to_s32_saturating((int64_t)input[i] * 24576, 15u);
		assert(v92_observation_point(&cache, input[i]).x == transformed[i]);
	}
	assert(v92_observation_predict(&rounded, input, 0.) == v92_observation_predict(model, transformed, 0.));
	cache.relative_noise = NAN;
	assert(!v92_observation_symbol(&cache, 0));
	cache.relative_noise = -1.;
	assert(!v92_observation_symbol(&cache, 0));
	rounded.quantizer_gain = DBL_TRUE_MIN;
	assert(v92_observation_is_valid(&rounded));
	v92_observation_begin(&cache, &rounded, 0.);
	assert(v92_observation_symbol(&cache, 0));
}

/* A noiseless final codec can move sample centres away from the correct
 * linear prediction. That alone does not justify another codec stage. */
static void check_single_codec_cells(enum g711_law law, double host_gain, bool translated)
{
	int32_t x[TRAINING];
	int16_t u[TRAINING], y[TRAINING], levels[TRAINING];
	double offsets[TRAINING];
	uint32_t state = 1;
	for (unsigned i = 0; i < TRAINING; i++) {
		int magnitude = (int)(2 * ((random_value(&state) >> 16) % 16) + 1) * 256;
		x[i] = random_value(&state) >> 31 ? -magnitude : magnitude;
		u[i] = (int16_t)x[i];
		double echo = translated ? (int)(random_value(&state) % 201) - 100 : 0.;
		levels[i] = quantize(law, x[i] / host_gain + echo);
		offsets[i] = -host_gain * echo;
		y[i] = (int16_t)round(host_gain * levels[i] + offsets[i]);
		struct g711_interval cell;
		assert(g711_quantization_interval(law, levels[i], &cell));
		assert(u[i] > (cell.lower - .5) * host_gain + offsets[i] &&
		       u[i] < (cell.upper + .5) * host_gain + offsets[i]);
	}
	struct v92_observation model, before;
	memset(&model, 0x5a, sizeof(model));
	before = model;
	const struct v92_observation_training training = {
		.input = x, .linear = u, .sample = y, .levels = levels, .offsets = offsets,
		.count = TRAINING, .taps = 3, .host_gain = host_gain, .law = law,
	};
	assert(!v92_observation_fit(&model, &training));
	assert(memcmp(&model, &before, sizeof(model)) == 0);
}

/* A small calibration bias can cross codec cells on a bipolar grid.
 * Rounded precoder inputs differ slightly from the linear prediction. The
 * actual wire observations still have a simple scalar explanation. */
static void check_scalar_codec_cells(enum g711_law law, bool translated)
{
	for (uint32_t seed = 1; seed <= 8; seed++) {
		int32_t x[TRAINING];
		int16_t u[TRAINING], y[TRAINING], levels[TRAINING];
		double offsets[TRAINING];
		uint32_t state = seed;
		const double host_gain = 1.3913323887692399;
		for (unsigned i = 0; i < TRAINING; i++) {
			int magnitude = (int)(2 * ((random_value(&state) >> 16) % 16) + 1) * 246 + 1;
			u[i] = random_value(&state) >> 31 ? magnitude : -magnitude;
			x[i] = u[i] + (int)(random_value(&state) % 3) - 1;
			offsets[i] = translated ? ((int)(random_value(&state) % 201) - 100) * .25 : 0.;
			levels[i] = quantize(law, (1.0015 * u[i] - offsets[i]) / host_gain);
			y[i] = (int16_t)lround(host_gain * levels[i] + offsets[i]);
		}
		struct v92_observation model, before;
		memset(&model, 0x5a, sizeof(model));
		memcpy(&before, &model, sizeof(before));
		const struct v92_observation_training training = {
			.input = x, .linear = u, .sample = y, .levels = levels, .offsets = offsets,
			.count = TRAINING, .taps = 3, .host_gain = host_gain, .law = law,
		};
		assert(!v92_observation_fit(&model, &training));
		assert(memcmp(&model, &before, sizeof(model)) == 0);
	}
}

/* Rounded interval endpoints can falsely contain a target whose actual
 * encoder reconstruction lies in the adjacent cell. A positive verdict must
 * survive the forward integer codec, in the active rounding context. */
static void check_unit_cell_boundary(void)
{
	const struct {
		enum g711_law law;
		int16_t linear, observed, level, adjacent;
		double gain, offset;
	} cases[] = {
		{ G711_LAW_PCMU, -16000, -15489, -31100, -32124, 1., 0x1.e7dbfffffffffp+13 },
		{ G711_LAW_PCMA, -1000, -961, -5504, -5760, 0x1.3333333333333p-2, 0x1.58dffffffffffp+9 },
	};
	const int modes[] = { FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO };
	int saved = fegetround();
	for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); ++c)
		for (unsigned m = 0; m < sizeof(modes) / sizeof(modes[0]); ++m) {
			assert(fesetround(modes[m]) == 0);
			int32_t x[4]; int16_t u[4], y[4], levels[4]; double offsets[4];
			volatile double gain = cases[c].gain, offset = cases[c].offset;
			for (unsigned i = 0; i < 4; ++i) {
				x[i] = u[i] = cases[c].linear; y[i] = cases[c].observed;
				levels[i] = cases[c].level; offsets[i] = offset;
			}
			struct v92_observation_training t = { .input = x, .linear = u,
				.sample = y, .levels = levels, .offsets = offsets, .count = 4,
				.taps = 3, .host_gain = gain, .law = cases[c].law };
			struct v92_observation_fit_work work;
			bool begun = v92_observation_fit_begin(&work, &t);
			enum v92_observation_fit_result result = V92_OBSERVATION_FIT_MORE;
			int16_t reconstructed = quantize(cases[c].law, (u[3] - offset) / gain);
			assert(begun || reconstructed == levels[3]);
			if (modes[m] == FE_TONEAREST) {
				assert(reconstructed == cases[c].adjacent && begun);
				struct v92_observation output, before;
				memset(&output, 0x5a, sizeof(output)); before = output;
				unsigned steps = 0;
				while (result == V92_OBSERVATION_FIT_MORE) {
					result = v92_observation_fit_step(&work, &t, &output);
					assert(++steps <= V92_SCALAR_FIT_STEPS && !memcmp(&output, &before, sizeof(output)));
				}
				assert(result == V92_OBSERVATION_FIT_DECLINED &&
				       work.scalar_result.status == V92_OBSERVATION_FIT_READY);
				assert(quantize(cases[c].law, (work.scalar_result.gain * u[3] - offset) / gain) == levels[3]);
			}
		}
	assert(fesetround(saved) == 0);
}

static void check_rebinding(const struct v92_observation *a, const struct v92_observation *b)
{
	struct v92_observation_cache cache;
	struct v92_observation rebound;
	/* Real fits at different gains exercise both wider buckets and direct
	 * integer lookup. Refresh dimensions even when model storage is reused
	 * between coding epochs rather than assigning a different address. */
	for (unsigned i = 0; i < 3; i++) {
		rebound = i == 1 ? *b : *a;
		v92_observation_begin(&cache, &rebound, 0.);
		check_geometry(&cache);
	}
}

static void check_retained_geometry(const struct v92_observation *a, const struct v92_observation *b)
{
	struct v92_observation_geometry geometry;
	struct v92_observation_cache retained[2], fresh;
	struct v92_observation rebound;
	const int modes[] = { FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO };
	int saved = fegetround();
	memset(&geometry, 0xa5, sizeof(geometry));
	for (unsigned epoch = 0; epoch < 3; epoch++) {
		rebound = epoch == 1 ? *b : *a;
		if (epoch == 1) rebound.input_coefficient = 24576;
		v92_observation_geometry_reset(&geometry);
		for (unsigned mode = 0; mode < sizeof(modes) / sizeof(*modes); mode++) {
			assert(fesetround(modes[mode]) == 0);
			for (unsigned frame = 0; frame < 2; frame++) {
				/* A different frame scratch address shares only model geometry.
				 * Its scores and symbol generation start afresh. */
				struct v92_observation_cache *cache = retained + frame;
				v92_observation_begin_cached(cache, &rebound, frame * .001, &geometry);
				v92_observation_begin(&fresh, &rebound, frame * .001);
				for (size_t i = 0; i < rebound.count; i++) {
					int32_t x = rebound.first[frame ? rebound.count - i - 1 : i];
					struct v92_observation_point p = v92_observation_point(cache, x);
					struct v92_observation_point q = v92_observation_point(&fresh, x);
					assert(p.x == q.x && p.key == q.key && p.span == q.span && p.values.stable == q.values.stable);
					assert(!memcmp(p.values.low, q.values.low, sizeof(p.values.low)) &&
						!memcmp(p.values.high, q.values.high, sizeof(p.values.high)));
					for (unsigned j = 0; j < V92_OBSERVATION_INTERVALS; j++)
						assert(p.values.cell[j].low == q.values.cell[j].low &&
							p.values.cell[j].high == q.values.cell[j].high &&
							p.values.cell[j].crossing == q.values.cell[j].crossing);
				}
			}
		}
	}
	assert(fesetround(saved) == 0);
	/* Ordinary local scratch remains movable: populating a copy must not
	 * mutate its source through a pointer into the original object. */
	memset(&fresh, 0xa5, sizeof(fresh));
	v92_observation_begin(&fresh, a, 0.);
	unsigned char before[sizeof(fresh)]; memcpy(before, &fresh, sizeof(fresh));
	memcpy(retained, &fresh, sizeof(fresh));
	(void)v92_observation_point(retained, INT32_MAX);
	assert(!memcmp(before, &fresh, sizeof(fresh)));
}

static void check_predictive_noise(const struct v92_observation *trained)
{
	const double gains[] = { .03125, 1., 2. };
	const double sigmas[] = { .01, 1., 16., 256., 10000. };
	const double shifts[] = { -40000., -1659.5, -3.5, 0., 3.5, 1659.5, 40000. };
	const int32_t input[3] = { 0 };
	struct v92_observation model = *trained;
	for (size_t g = 0; g < sizeof(gains) / sizeof(*gains); ++g)
		for (size_t s = 0; s < sizeof(sigmas) / sizeof(*sigmas); ++s)
			for (size_t o = 0; o < sizeof(shifts) / sizeof(*shifts); ++o) {
				model.quantizer_gain = gains[g];
				model.variance = fmax(1., sigmas[s] * sigmas[s] * gains[g] * gains[g]);
				assert(v92_observation_is_valid(&model));
				double expected, offset = gains[g] * shifts[o];
				independent_score(&model, input, 0, quantize(model.law, 0), offset, &expected);
				double actual = v92_observation_predict(&model, input, offset);
				assert(isfinite(actual) && fabs(actual - expected) < 1e-7);
			}
	/* At a mu-law boundary, symmetric uncertainty predicts the mean of
	 * neighbouring reconstructed levels, not either hard codec decision. */
	if (model.law == G711_LAW_PCMU) {
		model.quantizer_gain = 1.; model.variance = 1.;
		assert(fabs(v92_observation_predict(&model, input, -1659.5) - .5) < 1e-7);
	}
	model.quantizer_gain = DBL_TRUE_MIN;
	model.variance = DBL_MAX;
	assert(v92_observation_is_valid(&model));
	double tiny = v92_observation_predict(&model, input, 0.);
	assert(isfinite(tiny) && fabs(tiny) <= 65536. * DBL_TRUE_MIN);
	printf("V.92 predictive noise law=%u checks passed\n", model.law);
}

/* Compare the tail-sum predictor with a complete, independently enumerated
 * sum of codec-cell probabilities at every reconstructed level and boundary.
 * Include directed rounding, where small signed additions need not vanish. */
static void check_predictive_boundaries(const struct v92_observation *trained)
{
	const int modes[] = { FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO };
	const double sigmas[] = { 1., 32., 512. };
	const int32_t input[V92_OBSERVATION_TAPS] = { 0 };
	struct v92_observation model = *trained;
	int saved = fegetround();
	assert(saved != -1);
	model.quantizer_gain = 1.;
	for (unsigned i = 0; i < V92_OBSERVATION_INTERVALS; i++) {
		memset(model.intervals[i].coefficient, 0, sizeof(model.intervals[i].coefficient));
		model.intervals[i].coefficient[0] = 1.;
		model.intervals[i].weight = i == 0 ? 1. : 0.;
	}
	for (unsigned mode = 0; mode < sizeof(modes) / sizeof(modes[0]); mode++) {
		assert(fesetround(modes[mode]) == 0);
		for (unsigned s = 0; s < sizeof(sigmas) / sizeof(sigmas[0]); s++) {
			model.variance = sigmas[s] * sigmas[s];
			assert(v92_observation_is_valid(&model));
			for (unsigned code = 0; code < 256; code++) {
				int16_t level = g711_decode(model.law, (uint8_t)code);
				struct g711_interval bin;
				assert(g711_quantization_interval(model.law, level, &bin));
				double edge = bin.lower - .5;
				const double means[] = { level, edge, nextafter(edge, -INFINITY),
					nextafter(edge, INFINITY), bin.upper + .5 };
				for (unsigned j = 0; j < sizeof(means) / sizeof(means[0]); j++) {
					double zero = quantize(model.law, 0.);
					double offset = zero - means[j];
					double expected = independent_mean(model.law, zero - offset, sigmas[s]) + offset;
					double actual = v92_observation_predict(&model, input, offset);
					assert(isfinite(actual) && fabs(actual - expected) < 1e-7);
				}
			}
		}
	}
	assert(fesetround(saved) == 0);
}

/* Recompute the entire cost without any pruning budget. The independent
 * codec-cell integration also checks the two scored endpoints. */
static uint64_t training_score(const struct v92_observation *model,
	const struct v92_observation_training *training)
{
	struct v92_observation_cache cache;
	v92_observation_begin(&cache, model, 0.);
	uint64_t total = 0;
	for (size_t i = model->taps; i < training->count; i++) {
		double offset = training->offsets ? training->offsets[i] : 0.;
		assert(v92_observation_symbol_phase(&cache, training->sample[i], training->levels + i, offset,
			i % V92_OBSERVATION_PHASES));
		struct v92_observation_point previous[V92_OBSERVATION_TAPS - 1];
		int32_t input[V92_OBSERVATION_TAPS];
		input[0] = training->input[i];
		for (unsigned k = 1; k < model->taps; k++) {
			input[k] = training->input[i - k];
			previous[k - 1] = v92_observation_point(&cache, input[k]);
		}
		uint64_t cost = v92_observation_score(&cache, input[0], previous, UINT64_MAX);
		assert(cost != UINT64_MAX && cost <= UINT64_MAX - total);
		total += cost;
		if (i == model->taps || i + 1 == training->count) {
			double prediction;
			uint64_t reference = independent_score_phase(model, input, training->sample[i],
				training->levels[i], offset, i % V92_OBSERVATION_PHASES, &prediction);
			assert(cost <= reference + 1 && reference <= cost + 1);
		}
	}
	return total;
}

/* A single channel geometry serves all wire phases. The forward mean must
 * match a complete 256-code sum at every boundary, and likelihood must match
 * an independent integer preimage plus gain integral. */
static void check_periodic_codec(const struct v92_observation *trained)
{
	struct v92_observation model = *trained;
	const int32_t zero[V92_OBSERVATION_TAPS] = { 0 };
	model.lsb[1] = G711_LSB_ZERO; model.lsb[5] = G711_LSB_ONE;
	model.lsb_mask = (1u << 1) | (1u << 5);
	assert(v92_observation_is_valid(&model));
	assert(isnan(v92_observation_predict(&model, zero, 0.)));
	assert(isnan(v92_observation_predict_phase(&model, zero, 0., 6)));
	struct v92_observation_cache cache;
	v92_observation_begin(&cache, &model, 0.);
	assert(!v92_observation_symbol(&cache, 0));
	unsigned char before[sizeof(cache)]; memcpy(before, &cache, sizeof(cache));
	assert(!v92_observation_symbol_phase(&cache, 0, NULL, 0., 6));
	assert(!memcmp(before, &cache, sizeof(cache)));
	model.lsb_mask ^= 1; assert(!v92_observation_is_valid(&model)); model.lsb_mask ^= 1;
	model.lsb[2] = (enum g711_lsb_mode)3;
	assert(!v92_observation_is_valid(&model)); model.lsb[2] = G711_LSB_UNCHANGED;
	const int rounds[] = { FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO };
	const double sigmas[] = { 1., 32., 512. };
	for (unsigned i = 0; i < V92_OBSERVATION_INTERVALS; ++i) {
		memset(model.intervals[i].coefficient, 0, sizeof(model.intervals[i].coefficient));
		model.intervals[i].coefficient[0] = 1.;
		model.intervals[i].weight = i == 0 ? 1. : 0.;
	}
	int saved = fegetround(); assert(saved != -1);
	for (unsigned r = 0; r < sizeof(rounds) / sizeof(*rounds); ++r) {
		assert(fesetround(rounds[r]) == 0);
		for (unsigned sigma = 0; sigma < sizeof(sigmas) / sizeof(*sigmas); ++sigma) {
			model.variance = sigmas[sigma] * sigmas[sigma];
			for (unsigned code = 0; code < 256; ++code) {
				struct g711_interval bin;
				assert(g711_octet_interval(model.law, code, &bin));
				const double means[] = { g711_decode(model.law, code), bin.lower - .5, bin.upper + .5 };
				for (unsigned j = 0; j < sizeof(means) / sizeof(*means); ++j)
					for (unsigned phase = 0; phase < V92_OBSERVATION_PHASES; ++phase) {
						double offset = g711_quantize(model.law, 0) - means[j];
						double expected = independent_mean_lsb(model.law, means[j], sigmas[sigma], model.lsb[phase]) + offset;
						double actual = v92_observation_predict_phase(&model, zero, offset, phase);
						assert(isfinite(actual) && fabs(actual - expected) < 1e-7);
					}
			}
		}
	}
	assert(fesetround(saved) == 0);
	model = *trained;
	model.lsb[1] = G711_LSB_ZERO; model.lsb[5] = G711_LSB_ONE;
	model.lsb_mask = (1u << 1) | (1u << 5);
	const int32_t tuples[][V92_OBSERVATION_TAPS] = {
		{ 0 }, { -20, 8, -5 }, { 17000, -24000, 3700 }, { INT32_MIN, 0, INT32_MAX }
	};
	for (unsigned t = 0; t < sizeof(tuples) / sizeof(*tuples); ++t)
		for (unsigned phase = 0; phase < V92_OBSERVATION_PHASES; ++phase) {
			unsigned code = phase & 1 ? 128 : 255;
			code = model.lsb[phase] == G711_LSB_UNCHANGED ? code :
				(code & 254u) | (model.lsb[phase] == G711_LSB_ONE);
			int16_t level = g711_decode(model.law, code);
			double expected, offset = phase - 2.;
			int16_t observed = level + (int)offset;
			uint64_t score = independent_score_phase(&model, tuples[t], observed, level, offset, phase, &expected);
			v92_observation_begin(&cache, &model, 0.);
			assert(v92_observation_symbol_phase(&cache, observed, &level, offset, phase));
			struct v92_observation_point previous[V92_OBSERVATION_TAPS - 1];
			for (unsigned k = 1; k < model.taps; ++k)
				previous[k - 1] = v92_observation_point(&cache, tuples[t][k]);
			uint64_t actual = v92_observation_score(&cache, tuples[t][0], previous, UINT64_MAX);
			assert(fabs(v92_observation_predict_phase(&model, tuples[t], offset, phase) - expected) < 1e-7);
			if (score < (uint64_t)(-2e6 * log(1e-300)))
				assert(actual <= score + 1 && score <= actual + 1);
		}
	printf("V.92 periodic codec law=%u checks passed\n", model.law);
}

/* Interleave different acquisition spans, suspend and restart one owner,
 * and ensure an incomplete job never publishes a decoder model. */
static void check_resumable(const struct v92_observation_training *training,
	const struct v92_observation *expected)
{
	struct v92_observation_training shorter = *training;
	shorter.count--;
	struct v92_observation other, output[2], before[2];
	struct v92_observation_fit_work work[3], *active[2] = { work, work + 1 };
	assert(v92_observation_fit(&other, &shorter));
	memset(output, 0x5a, sizeof(output));
	memcpy(before, output, sizeof(before));
	assert(v92_observation_fit_begin(active[0], training));
	assert(v92_observation_fit_begin(active[1], &shorter));
	/* A bad wire cell must not leave a resumable job using the preceding
	 * acquisition's bounds. Include unused history and the scored tail,
	 * then restart beside the untouched first owner. */
	int16_t levels[TRAINING];
	const size_t invalid[] = { 0, shorter.taps, shorter.count - 1 };
	for (unsigned j = 0; j < sizeof(invalid) / sizeof(invalid[0]); j++) {
		memcpy(levels, shorter.levels, shorter.count * sizeof(*levels));
		levels[invalid[j]] = 1; /* Neither law reconstructs this level. */
		struct v92_observation_training bad = shorter;
		bad.levels = levels;
		assert(!v92_observation_fit_begin(active[1], &bad));
		assert(active[1]->training_cost == UINT64_MAX);
		assert(!memcmp(output + 1, before + 1, sizeof(*output)));
		assert(v92_observation_fit_begin(active[1], &shorter));
	}
	bool done[2] = { false, false };
	unsigned steps[2] = { 0, 0 };
	for (unsigned turn = 0; !done[0] || !done[1]; turn++) {
		unsigned i = turn % 3 == 0 ? 1 : 0;
		if (done[i]) continue;
		const struct v92_observation_training *t = i ? &shorter : training;
		assert(active[i]->training_cost == UINT64_MAX);
		enum v92_observation_fit_result result = v92_observation_fit_step(active[i], t, output + i);
		assert(++steps[i] < 128);
		assert(result != V92_OBSERVATION_FIT_DECLINED);
		if (result == V92_OBSERVATION_FIT_MORE) {
			assert(active[i]->training_cost == UINT64_MAX);
			assert(!memcmp(output + i, before + i, sizeof(*output)));
			if (i == 1 && steps[i] == 3)
				assert(v92_observation_fit_begin(active[i], t));
			if (i == 1 && steps[i] == 5) {
				work[2] = *active[i]; memset(active[i], 0, sizeof(*active[i]));
				active[i] = work + 2;
			}
		} else {
			done[i] = true;
			assert(steps[i] > 3 && v92_observation_is_valid(output + i));
			assert(!memcmp(output + i, i ? &other : expected, sizeof(*output)));
			assert(active[i]->training_cost == training_score(output + i, t));
		}
	}
	struct v92_observation_training invalid_training = *training;
	invalid_training.count = 0;
	uint64_t previous_cost = active[0]->training_cost;
	assert(!v92_observation_fit_begin(active[0], &invalid_training));
	assert(active[0]->training_cost == previous_cost);
	if (!training->offsets) {
		struct v92_observation_training inferred = *training;
		inferred.levels = NULL;
		assert(v92_observation_fit_begin(active[0], &inferred));
		enum v92_observation_fit_result result;
		do {
			result = v92_observation_fit_step(active[0], &inferred, output);
			assert(active[0]->training_cost == UINT64_MAX);
		} while (result == V92_OBSERVATION_FIT_MORE);
		assert(result == V92_OBSERVATION_FIT_READY && !memcmp(output, expected, sizeof(*output)));
	}
}

/* Identical input magnitudes put every codec transition at the same gain.
 * Even a singular channel must yield between transition batches, without
 * exposing a partial model or depending only on distinct-cell count. */
static void check_coincident_fit(enum g711_law law)
{
	int32_t x[TRAINING];
	int16_t y[TRAINING], u[TRAINING] = { 0 };
	for (unsigned i = 0; i < TRAINING; i++) { x[i] = 4096; y[i] = (int16_t)(i * 8); }
	struct v92_observation_training training = {
		.input = x, .sample = y, .linear = u, .count = TRAINING,
		.taps = 3, .host_gain = 1., .law = law,
	};
	struct v92_observation_fit_work work;
	struct v92_observation output, before;
	memset(&output, 0x5a, sizeof(output)); before = output;
	assert(v92_observation_fit_begin(&work, &training));
	unsigned steps = 0;
	enum v92_observation_fit_result result;
	do {
		result = v92_observation_fit_step(&work, &training, &output);
		assert(work.training_cost == UINT64_MAX);
		assert(++steps < 128 && !memcmp(&output, &before, sizeof(output)));
	} while (result == V92_OBSERVATION_FIT_MORE);
	assert(result == V92_OBSERVATION_FIT_DECLINED && steps > 2);
}

/* A noisy single-codec channel reaches final admission, where the tandem
 * candidate loses to linear. Its pruned sum must not become an exact cost. */
static void check_pruned_cost(void)
{
	int32_t x[TRAINING];
	int16_t y[TRAINING], u[TRAINING];
	uint32_t state = 7;
	for (unsigned i = 0; i < TRAINING; i++) {
		x[i] = (int32_t)(random_value(&state) % 40001) - 20000;
		double mean = .63 * x[i] + (i ? .07 * x[i - 1] : 0.) -
			(i > 1 ? .02 * x[i - 2] : 0.);
		u[i] = (int16_t)lround(mean);
		y[i] = quantize(G711_LAW_PCMU, mean + (int)(random_value(&state) % 129) - 64);
	}
	const struct v92_observation_training training = {
		.input = x, .linear = u, .sample = y, .levels = y,
		.count = TRAINING, .taps = 3, .host_gain = 1., .law = G711_LAW_PCMU,
	};
	struct v92_observation_fit_work work;
	struct v92_observation output, before;
	memset(&output, 0xa5, sizeof(output)); before = output;
	assert(v92_observation_fit_begin(&work, &training));
	enum v92_observation_fit_result result;
	unsigned stage;
	do {
		stage = work.stage;
		result = v92_observation_fit_step(&work, &training, &output);
		assert(work.training_cost == UINT64_MAX);
		assert(!memcmp(&output, &before, sizeof(output)));
	} while (result == V92_OBSERVATION_FIT_MORE);
	assert(stage == V92_FIT_VALIDATE && result == V92_OBSERVATION_FIT_DECLINED);
}

static void check_channel(enum g711_law law, double gain, unsigned taps,
	size_t count, struct v92_observation *previous)
{
	int32_t x[SAMPLES];
	int16_t q[SAMPLES], y[SAMPLES], u[TRAINING] = { 0 };
	uint32_t state = UINT32_C(0x182dab72);
	for (unsigned i = 0; i < SAMPLES; i++) {
		x[i] = (int32_t)round(((int)(random_value(&state) % 40001) - 20000) / gain);
		q[i] = quantize(law, gain * x[i]);
		double sample = .63 * q[i] + (i ? .07 * q[i - 1] : 0.) - (i > 1 ? .02 * q[i - 2] : 0.);
		if (taps == 4 && i > 2) sample += .03 * q[i - 3];
		y[i] = quantize(law, sample);
		if (i < TRAINING)
			u[i] = (int16_t)lround(gain * (.63 * x[i] + (i ? .07 * x[i - 1] : 0.) -
				(i > 1 ? .02 * x[i - 2] : 0.) + (taps == 4 && i > 2 ? .03 * x[i - 3] : 0.)));
	}
	struct v92_observation model;
	const struct v92_observation_training training = {
		.input = x, .linear = u, .sample = y, .levels = y,
		.count = count, .taps = taps, .host_gain = 1., .law = law,
	};
	assert(v92_observation_fit(&model, &training));
	assert(v92_observation_is_valid(&model));
	if (gain == 4. && count == TRAINING) check_resumable(&training, &model);
	struct v92_observation before = model;
	struct v92_observation inferred;
	struct v92_observation_training without_levels = training;
	without_levels.levels = NULL;
	assert(v92_observation_fit(&inferred, &without_levels));
	assert(memcmp(&inferred, &model, sizeof(model)) == 0);
	double error = 0.;
	for (size_t i = TRAINING; i < SAMPLES; i++) {
		int32_t input[] = { x[i], x[i - 1], x[i - 2], x[i - 3] };
		double residual = v92_observation_predict(&model, input, 0.) - y[i];
		error += residual * residual;
	}
	double rms = sqrt(error / (SAMPLES - TRAINING));
	assert(isfinite(rms) && rms < 100.);
	check_scores(&model, x, y);
	check_history_scores(&model, x, y);
	if (gain == 4. && count == TRAINING) {
		check_coupled_bounds(&model, false);
		check_coupled_bounds(&model, true);
	}
	if (gain == .125 && taps == 3) {
		check_predictive_noise(&model);
		check_predictive_boundaries(&model);
		check_periodic_codec(&model);
		check_tail_scores(&model);
		check_tail_cells(&model);
		check_integer_scores(&model);
	}
	assert(memcmp(&model, &before, sizeof(model)) == 0);
	check_failures(&model, x, u, y);
	if (previous->count) {
		check_rebinding(previous, &model);
		check_retained_geometry(previous, &model);
	}
	*previous = model;
	printf("V.92 observation law=%u taps=%u gain=%.9g count=%zu heldout_rms=%.6f\n", law, taps, gain, count, rms);
}

/* Fit only the known prefix. Each physical phase has a separate positive
 * wire-operation case; unknown later inputs test the frozen reconstruction.
 * Incremental publication and its exact cost must agree with synchronous fit. */
static void check_periodic_training(enum g711_law law, unsigned taps,
	enum g711_lsb_mode operation, unsigned phase)
{
	int32_t x[SAMPLES];
	int16_t q[SAMPLES], y[SAMPLES], linear[SAMPLES];
	enum g711_lsb_mode pattern[V92_OBSERVATION_PHASES] = { 0 };
	uint32_t seed = UINT32_C(0x182dab72);
	pattern[phase] = operation;
	for (unsigned i = 0; i < SAMPLES; i++) {
		x[i] = (int32_t)round(((int)(random_value(&seed) % 40001) - 20000) / 4.);
		q[i] = quantize(law, 4. * x[i]);
		double observed = .63 * q[i] + (i ? .07 * q[i - 1] : 0.) -
			(i > 1 ? .02 * q[i - 2] : 0.) + (taps == 4 && i > 2 ? .03 * q[i - 3] : 0.);
		double baseline = 4. * (.63 * x[i] + (i ? .07 * x[i - 1] : 0.) -
			(i > 1 ? .02 * x[i - 2] : 0.) + (taps == 4 && i > 2 ? .03 * x[i - 3] : 0.));
		assert(fabs(observed) < INT16_MAX && fabs(baseline) < INT16_MAX);
		y[i] = g711_decode(law, g711_apply_lsb(g711_encode(law, (int16_t)round(observed)),
			pattern[i % V92_OBSERVATION_PHASES]));
		linear[i] = (int16_t)round(baseline);
	}
	const struct v92_observation_training training = {
		.input = x, .linear = linear, .sample = y, .levels = y,
		.count = TRAINING, .taps = taps, .host_gain = 1., .law = law, .lsb = pattern,
	};
	struct v92_observation model, whole, before;
	struct v92_observation_fit_work work;
	memset(&model, 0x5a, sizeof(model)); before = model;
	assert(v92_observation_fit_begin(&work, &training));
	enum v92_observation_fit_result result;
	unsigned steps = 0;
	do {
		result = v92_observation_fit_step(&work, &training, &model);
		assert(++steps < 128);
		if (result == V92_OBSERVATION_FIT_MORE)
			assert(!memcmp(&model, &before, sizeof(model)) && work.training_cost == UINT64_MAX);
	} while (result == V92_OBSERVATION_FIT_MORE);
	assert(result == V92_OBSERVATION_FIT_READY && v92_observation_is_valid(&model));
	assert(model.lsb_mask == (1u << phase) && !memcmp(model.lsb, pattern, sizeof(pattern)));
	assert(v92_observation_fit(&whole, &training) && !memcmp(&whole, &model, sizeof(model)));
	assert(work.training_cost == training_score(&model, &training));
	double error = 0., wrong_phase_error = 0.;
	for (unsigned i = TRAINING; i < SAMPLES; i++) {
		int32_t input[V92_OBSERVATION_TAPS];
		for (unsigned k = 0; k < taps; k++) input[k] = x[i - k];
		double prediction = v92_observation_predict_phase(&model, input, 0., i % V92_OBSERVATION_PHASES);
		double wrong = v92_observation_predict_phase(&model, input, 0., (i + 1) % V92_OBSERVATION_PHASES);
		assert(isfinite(prediction) && isfinite(wrong));
		error += (prediction - y[i]) * (prediction - y[i]);
		wrong_phase_error += (wrong - y[i]) * (wrong - y[i]);
	}
	assert(sqrt(error / (SAMPLES - TRAINING)) < 128. && error < wrong_phase_error);
}

/* Independently integrate the actual final-codec cells. A retained fitted
 * channel must beat a feasible channel with unit noise, including when the
 * optimum lies on that noise floor. No optimizer or cached score is used. */
static double physical_cell_loss(enum g711_law law, double mean, double sigma,
	int16_t level, double gain, double offset)
{
	struct g711_interval cell;
	assert(g711_quantization_interval(law, level, &cell));
	double a = cell.lower == INT16_MIN ? -HUGE_VAL :
		((cell.lower - .5) * gain + offset - mean) / (sqrt(2.) * sigma);
	double b = cell.upper == INT16_MAX ? HUGE_VAL :
		((cell.upper + .5) * gain + offset - mean) / (sqrt(2.) * sigma);
	double probability = a > 0. ? .5 * (erfc(a) - erfc(b)) :
		b < 0. ? .5 * (erfc(-b) - erfc(-a)) : .5 * (erf(b) - erf(a));
	return probability > 0. ? -log(probability) : HUGE_VAL;
}

static void check_fit_witness(const struct v92_observation *model, const int32_t *x,
	const int16_t *wire, const double *offsets, double host_gain)
{
	double best = HUGE_VAL, witness = HUGE_VAL;
	const double coefficient[V92_OBSERVATION_TAPS] = { .63, .07, -.02 };
	for (unsigned i = 0; i < V92_OBSERVATION_INTERVALS; i++) {
		const struct v92_observation_interval *fit = &model->intervals[i];
		double gain = sqrt(fit->low * fit->high), loss = 0., reference = 0.;
		for (unsigned j = model->taps; j < TRAINING; j++) {
			double mean = 0., known = 0.;
			for (unsigned k = 0; k < model->taps; k++) {
				int16_t point = quantize(model->law, gain * x[j - k]);
				mean += fit->coefficient[k] * point;
				known += host_gain * coefficient[k] * .125 / gain * point;
			}
			loss += physical_cell_loss(model->law, mean, sqrt(model->variance),
				wire[j], host_gain, offsets[j]);
			reference += physical_cell_loss(model->law, known, 1., wire[j], host_gain, offsets[j]);
		}
		best = fmin(best, loss);
		witness = fmin(witness, reference);
	}
	assert(isfinite(best) && isfinite(witness) && best <= witness + 1e-6 * (1. + witness));
}

static void check_conditioned_channel(enum g711_law law, double host_gain)
{
	int32_t x[SAMPLES];
	int16_t q[SAMPLES], wire[SAMPLES], y[SAMPLES], u[TRAINING] = { 0 };
	double offsets[SAMPLES];
	uint32_t state = 0x471921abu;
	for (unsigned i = 0; i < SAMPLES; i++) {
		x[i] = 8 * ((int)(random_value(&state) % 40001) - 20000);
		q[i] = quantize(law, .125 * x[i]);
		double echo = 4 * ((int)(random_value(&state) % 201) - 100);
		double signal = .63 * q[i] + (i ? .07 * q[i - 1] : 0.) - (i > 1 ? .02 * q[i - 2] : 0.);
		wire[i] = quantize(law, signal + echo);
		offsets[i] = -host_gain * echo;
		y[i] = (int16_t)round(host_gain * wire[i] + offsets[i]);
		if (i < TRAINING)
			u[i] = (int16_t)lround(host_gain * .125 * (.63 * x[i] +
				(i ? .07 * x[i - 1] : 0.) - (i > 1 ? .02 * x[i - 2] : 0.)));
	}
	struct v92_observation model;
	const struct v92_observation_training training = {
		.input = x, .linear = u, .sample = y, .levels = wire, .offsets = offsets,
		.count = TRAINING, .taps = 3, .host_gain = host_gain, .law = law,
	};
	assert(v92_observation_fit(&model, &training));
	check_fit_witness(&model, x, wire, offsets, host_gain);
	check_resumable(&training, &model);
	struct v92_observation_cache cache;
	/* Transaction checks compare unused scratch slots too. */
	memset(&cache, 0xa5, sizeof(cache));
	v92_observation_begin(&cache, &model, 0.);
	double error = 0., unconditioned = 0.;
	for (unsigned i = TRAINING; i < SAMPLES; i++) {
		int32_t input[] = { x[i], x[i - 1], x[i - 2] };
		assert(v92_observation_symbol_level(&cache, y[i], wire + i, offsets[i]));
		double residual = v92_observation_predict(&model, input, offsets[i]) - y[i];
		error += residual * residual;
		residual = v92_observation_predict(&model, input, 0.) - y[i];
		unconditioned += residual * residual;
		if (i < TRAINING + 8) {
			double prediction;
			uint64_t expected = independent_score(&model, input, y[i], wire[i], offsets[i], &prediction);
			struct v92_observation_point b = v92_observation_point(&cache, input[1]);
			struct v92_observation_point c = v92_observation_point(&cache, input[2]);
			uint64_t actual = v92_observation_score(&cache, input[0], (struct v92_observation_point[]){ b, c }, UINT64_MAX);
			assert(actual <= expected + 1 && expected <= actual + 1);
			assert(fabs(v92_observation_predict(&model, input, offsets[i]) - prediction) < 1e-7);
		}
	}
	assert(error < unconditioned && sqrt(error / (SAMPLES - TRAINING)) < 100. * host_gain);
	/* Final-codec saturation retains one-sided bins after translation; it
	 * is not host clipping after the correction (which the caller excludes). */
	for (int sign = -1; sign <= 1; sign += 2) {
		int16_t level = quantize(law, sign * 32767.);
		int16_t observed = (int16_t)(sign * 30000);
		double offset = observed - host_gain * level;
		assert(v92_observation_symbol_level(&cache, observed, &level, offset));
		assert(sign < 0 ? isinf(cache.lower) && cache.lower < 0. : isinf(cache.upper) && cache.upper > 0.);
	}
	struct v92_observation_cache before = cache;
	const double bad[] = { NAN, INFINITY, -INFINITY };
	for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		assert(!v92_observation_symbol_level(&cache, y[0], wire, bad[i]));
		assert(!memcmp(&cache, &before, sizeof(cache)));
		assert(isnan(v92_observation_predict(&model, x, bad[i])));
	}
	assert(!v92_observation_symbol_level(&cache, 0, NULL, 1.));
	assert(!v92_observation_symbol_level(&cache, y[0], wire, DBL_MAX));
	assert(!v92_observation_symbol_level(&cache, y[0], wire, -DBL_MAX));
	assert(!memcmp(&cache, &before, sizeof(cache)));
	printf("V.92 conditioned law=%u host=%.2f heldout_rms=%.6f unconditioned_rms=%.6f\n",
		law, host_gain, sqrt(error / (SAMPLES - TRAINING)), sqrt(unconditioned / (SAMPLES - TRAINING)));
}

static void check_noise_fit(enum g711_law law, unsigned sigma)
{
	int32_t x[TRAINING];
	int16_t q[TRAINING], y[TRAINING], u[TRAINING] = { 0 };
	uint32_t seed = UINT32_C(0x32acdc78);
	double noise_energy = 0.;
	for (unsigned j = 0; j < TRAINING; j++) {
		x[j] = 8 * ((int)(random_value(&seed) % 40001) - 20000);
		q[j] = quantize(law, .125 * x[j]);
		/* Independent Gaussian samples, injected before the final codec.
		 * Open uniforms keep Box-Muller finite at every generator value. */
		double a = (random_value(&seed) + .5) / 4294967296.;
		double b = (random_value(&seed) + .5) / 4294967296.;
		double noise = sigma * sqrt(-2. * log(a)) * cos(2. * acos(-1.) * b);
		y[j] = quantize(law, .63 * q[j] + (j ? .07 * q[j - 1] : 0.) -
			(j > 1 ? .02 * q[j - 2] : 0.) + noise);
		u[j] = (int16_t)lround(.125 * (.63 * x[j] + (j ? .07 * x[j - 1] : 0.) -
			(j > 1 ? .02 * x[j - 2] : 0.)));
		if (j >= 3) noise_energy += noise * noise;
	}
	struct v92_observation model;
	const struct v92_observation_training training = {
		.input = x, .linear = u, .sample = y, .levels = y,
		.count = TRAINING, .taps = 3, .host_gain = 1., .law = law,
	};
	assert(v92_observation_fit(&model, &training));
	assert(v92_observation_is_valid(&model));
	double measured = noise_energy / (TRAINING - 3);
	/* Allow finite-training uncertainty, but reject the severe downward
	 * bias from treating predictions inside a codec bin as noise-free. */
	assert(model.variance > .5 * measured && model.variance < 2. * measured);
	printf("V.92 noise law=%u sigma=%u measured=%.6f fitted=%.6f\n",
		law, sigma, measured, model.variance);
}

/* Interleave two scalar owners under each rounding mode. Restart a partial
 * job after bad wire evidence, and move the other without retaining pointers
 * into its discarded storage. Nothing may publish before the final batch. */
static void check_scalar_resumable(const struct v92_observation_training *training)
{
	const int modes[] = { FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO };
	int saved = fegetround();
	for (unsigned mode = 0; mode < sizeof(modes) / sizeof(modes[0]); mode++) {
		assert(fesetround(modes[mode]) == 0);
		struct v92_observation_training t[2] = { *training, *training };
		t[1].count--;
		struct v92_observation_fit_work work[3], *active[2] = { work, work + 1 };
		double expected[2] = { -1., -1. }, actual[2] = { -1., -1. };
		bool accepted[2], done[2] = { false, false }, restarted = false, moved = false;
		unsigned steps[2] = { 0, 0 };
		for (unsigned i = 0; i < 2; i++) {
			accepted[i] = v92_observation_fit_scalar(expected + i, t + i);
			assert(v92_observation_scalar_begin(active[i], t + i));
		}
		for (unsigned turn = 0; !done[0] || !done[1]; turn++) {
			unsigned i = turn % 3 == 0 ? 1 : 0;
			if (done[i]) continue;
			unsigned before = active[i]->scalar.fit.iterations;
			enum v92_observation_fit_result result = v92_observation_scalar_step(active[i], t + i, actual + i);
			assert(active[i]->training_cost == UINT64_MAX);
			assert(active[i]->scalar.fit.iterations - before <= V92_SCALAR_FIT_BATCH);
			assert(++steps[i] <= V92_SCALAR_FIT_STEPS);
			if (result == V92_OBSERVATION_FIT_MORE) {
				assert(actual[i] == -1.);
				assert(active[i]->scalar_result.status == V92_OBSERVATION_FIT_MORE &&
				       active[i]->scalar_result.gain == 0.);
				if (i == 0 && steps[i] == 2 && !restarted) {
					int16_t levels[TRAINING];
					memcpy(levels, t[i].levels, t[i].count * sizeof(*levels));
					levels[0] = 1;
					struct v92_observation_training bad = t[i]; bad.levels = levels;
					assert(!v92_observation_scalar_begin(active[i], &bad));
					assert(v92_observation_scalar_begin(active[i], t + i));
					restarted = true; steps[i] = 0;
				} else if (i == 1 && steps[i] == 3 && !moved) {
					work[2] = *active[i]; memset(active[i], 0, sizeof(*active[i]));
					active[i] = work + 2; moved = true;
				}
			} else {
				assert((result == V92_OBSERVATION_FIT_READY) == accepted[i]);
				assert(memcmp(actual + i, expected + i, sizeof(*actual)) == 0);
				assert(active[i]->scalar_result.status == result);
				assert(active[i]->scalar_result.gain == (accepted[i] ? actual[i] : 0.));
				assert(active[i]->scalar_result.rounding == modes[mode] &&
				       v92_scalar_fit_result_can_reuse(active[i]->scalar_result));
				if (accepted[i]) assert(steps[i] == V92_SCALAR_FIT_STEPS);
				done[i] = true;
			}
		}
		assert(restarted && moved);
	}
	assert(fesetround(saved) == 0);
}

/* One physical codec after a linear source and independently known echo.
 * The fit only sees the training prefix; different later points and echo
 * validate prediction without pretending quantized centres are noiseless. */
static enum v92_observation_fit_result finish_fit(
	struct v92_observation_fit_work *work,
	const struct v92_observation_training *training,
	struct v92_observation *output, enum v92_observation_fit_result result,
	unsigned *scalar_steps)
{
	unsigned steps = 0;
	while (result == V92_OBSERVATION_FIT_MORE) {
		*scalar_steps += work->stage == V92_FIT_SCALAR;
		result = v92_observation_fit_step(work, training, output);
		assert(++steps <= 256);
	}
	return result;
}

/* A completed admission result remains useful after the tandem fit declines.
 * Reuse it with a different first-codec input, not different physical cells. */
static void check_scalar_reuse(const struct v92_observation_training *training)
{
	const int modes[] = { FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO };
	int rounding = fegetround();
	for (unsigned mode = 0; mode < sizeof(modes) / sizeof(modes[0]); mode++) {
		assert(fesetround(modes[mode]) == 0);
		struct v92_observation_fit_work *work = calloc(3, sizeof(*work));
		struct v92_observation output[3] = { 0 };
		struct v92_observation_training other = *training;
		int32_t input[TRAINING];
		unsigned steps[3] = { 0 };
		assert(work);
		for (unsigned i = 0; i < training->count; i++)
			input[i] = round_shift_s64_to_s32_saturating((int64_t)training->input[i] * 13753, 15);
		other.input = input;
		bool begun = v92_observation_fit_begin(work, training);
		assert(work->scalar_result.status == V92_OBSERVATION_FIT_MORE);
		finish_fit(work, training, output, begun ? V92_OBSERVATION_FIT_MORE :
			V92_OBSERVATION_FIT_DECLINED, steps);
		struct v92_scalar_fit_result completed = work->scalar_result;
		assert(completed.status == V92_OBSERVATION_FIT_READY);
		assert(v92_scalar_fit_result_can_reuse(completed) && completed.rounding == modes[mode]);
		double scalar = 0.;
		assert(v92_observation_fit_scalar(&scalar, training) &&
		       memcmp(&scalar, &completed.gain, sizeof(scalar)) == 0);
		/* A new job may have another rounding mode. It must not consume
		 * a completed result from the previous numerical context. */
		assert(fesetround(modes[(mode + 1u) % 4u]) == 0);
		assert(!v92_scalar_fit_result_can_reuse(completed) &&
		       v92_observation_fit_begin(work + 2, &other));
		work[0] = work[2];
		assert(v92_observation_fit_finish_scalar_owned(work + 2, &other,
			completed) == V92_OBSERVATION_FIT_DECLINED &&
		       memcmp(work, work + 2, sizeof(*work)) == 0);
		assert(fesetround(modes[mode]) == 0 && v92_scalar_fit_result_can_reuse(completed));
		assert(v92_observation_fit_begin(work + 1, &other) &&
		       v92_observation_fit_begin(work + 2, &other) &&
		       work[1].stage == V92_FIT_SCALAR && work[2].stage == V92_FIT_SCALAR);

		/* A value copy must survive destruction of its former owner. Invalid
		 * or unavailable evidence and invalid begin preserve the new job. */
		work[0] = work[2];
		const struct v92_scalar_fit_result invalid[] = {
			{ .status = V92_OBSERVATION_FIT_MORE },
			{ .status = (enum v92_observation_fit_result)-1 },
			{ .status = V92_OBSERVATION_FIT_READY, .gain = 0. },
			{ .status = V92_OBSERVATION_FIT_READY, .gain = -1. },
			{ .status = V92_OBSERVATION_FIT_READY, .gain = NAN },
			{ .status = V92_OBSERVATION_FIT_READY, .gain = INFINITY },
			{ .status = V92_OBSERVATION_FIT_DECLINED, .gain = 1. },
			{ .status = V92_OBSERVATION_FIT_READY, .gain = 1., .rounding = -1 },
		};
		for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
			assert(v92_observation_fit_finish_scalar_owned(work + 2, &other,
				invalid[i]) == V92_OBSERVATION_FIT_DECLINED);
			assert(memcmp(work, work + 2, sizeof(*work)) == 0);
		}
		struct v92_observation_training bad = other;
		bad.count = 0;
		assert(!v92_observation_fit_begin(work + 2, &bad) &&
		       memcmp(work, work + 2, sizeof(*work)) == 0);
		enum v92_observation_fit_result reused = v92_observation_fit_finish_scalar_owned(
			work + 2, &other, completed);
		enum v92_observation_fit_result fresh = finish_fit(work + 1, &other,
			output + 1, V92_OBSERVATION_FIT_MORE, steps + 1);
		reused = finish_fit(work + 2, &other, output + 2, reused, steps + 2);
		assert(fresh == reused && steps[1] == V92_SCALAR_FIT_STEPS && steps[2] == 0);
		assert(memcmp(output + 1, output + 2, sizeof(*output)) == 0 &&
		       work[1].training_cost == work[2].training_cost &&
		       work[1].scalar_result.status == work[2].scalar_result.status &&
		       memcmp(&work[1].scalar_result.gain, &work[2].scalar_result.gain,
			      sizeof(completed.gain)) == 0);
		assert(v92_observation_scalar_begin(work + 2, &other) &&
		       work[2].scalar_result.status == V92_OBSERVATION_FIT_MORE &&
		       work[2].scalar_result.gain == 0.);
		free(work);
	}
	assert(fesetround(rounding) == 0);
}

/* Only the last sample constrains scalar admission after the causal prefix.
 * The full scalar solve is dominated by the other three exact codewords and
 * cannot improve reconstruction. Its DECLINED is a real completed result. */
static void check_scalar_decline_reuse(enum g711_law law)
{
	int16_t u[4], y[4];
	int32_t x[4], other[4];
	const int values[] = { 16000, 12000, -14000, 1000 };
	for (unsigned i = 0; i < 4; i++) {
		x[i] = u[i] = y[i] = quantize(law, values[i]);
		other[i] = 2 * x[i];
	}
	y[3] = quantize(law, u[3] - (law == G711_LAW_PCMU ? 33 : 17));
	struct v92_observation_training t = { .input = x, .linear = u,
		.sample = y, .levels = y, .count = 4, .taps = 3, .host_gain = 1., .law = law };
	struct v92_observation_fit_work *work = calloc(3, sizeof(*work));
	struct v92_observation output[3] = { 0 };
	unsigned steps[3] = { 0 };
	assert(work && v92_observation_fit_begin(work, &t) && work->stage == V92_FIT_SCALAR);
	finish_fit(work, &t, output, V92_OBSERVATION_FIT_MORE, steps);
	struct v92_scalar_fit_result completed = work->scalar_result;
	assert(completed.status == V92_OBSERVATION_FIT_DECLINED && completed.gain == 0.);
	double gain = -1.;
	assert(!v92_observation_fit_scalar(&gain, &t) && gain == -1. &&
	       v92_observation_scalar_begin(work, &t));
	enum v92_observation_fit_result result;
	do { result = v92_observation_scalar_step(work, &t, &gain); }
	while (result == V92_OBSERVATION_FIT_MORE);
	assert(result == V92_OBSERVATION_FIT_DECLINED && gain == -1. &&
	       work->scalar_result.status == V92_OBSERVATION_FIT_DECLINED);
	t.input = other;
	assert(v92_observation_fit_begin(work + 1, &t) &&
	       v92_observation_fit_begin(work + 2, &t) && work[2].stage == V92_FIT_SCALAR);
	enum v92_observation_fit_result reused = v92_observation_fit_finish_scalar_owned(
		work + 2, &t, completed);
	assert(reused == V92_OBSERVATION_FIT_MORE && work[2].stage == V92_FIT_SWEEP);
	result = finish_fit(work + 1, &t, output + 1, V92_OBSERVATION_FIT_MORE, steps + 1);
	reused = finish_fit(work + 2, &t, output + 2, reused, steps + 2);
	assert(result == reused && steps[1] == V92_SCALAR_FIT_STEPS && steps[2] == 0 &&
	       memcmp(output + 1, output + 2, sizeof(*output)) == 0 &&
	       work[1].training_cost == work[2].training_cost);
	/* A generic linear-explained begin decline grants no scalar result. */
	t.sample = t.levels = u;
	assert(!v92_observation_fit_begin(work, &t) &&
	       work->scalar_result.status == V92_OBSERVATION_FIT_MORE &&
	       work->scalar_result.gain == 0.);
	free(work);
}

static void check_scalar(enum g711_law law, double host_gain, enum g711_lsb_mode mode)
{
	int32_t x[SAMPLES];
	int16_t u[SAMPLES], y[SAMPLES], levels[SAMPLES];
	double offsets[SAMPLES];
	uint32_t seed = UINT32_C(0x4368217b);
	const double actual_gain = .9983;
	enum g711_lsb_mode lsb[V92_OBSERVATION_PHASES] = { [1] = mode };
	for (unsigned i = 0; i < SAMPLES; i++) {
		x[i] = u[i] = (int16_t)((int)(random_value(&seed) % 40001) - 20000);
		double echo = (int)(random_value(&seed) % 1401) - 700;
		levels[i] = quantize(law, actual_gain * u[i] / host_gain + echo);
		if (i % V92_OBSERVATION_PHASES == 1)
			levels[i] = g711_decode(law, g711_apply_lsb(g711_encode(law, levels[i]), mode));
		double corrected = host_gain * (levels[i] - echo);
		y[i] = (int16_t)lround(corrected);
		offsets[i] = -host_gain * echo;
	}
	const struct v92_observation_training training = {
		.input = x, .linear = u, .sample = y, .levels = levels, .offsets = offsets,
		.count = TRAINING, .taps = 3, .host_gain = host_gain, .law = law,
		.lsb = mode == G711_LSB_UNCHANGED ? NULL : lsb,
	};
	double fitted = 0.;
	assert(v92_observation_fit_scalar(&fitted, &training));
	assert(fabs(fitted - actual_gain) < .002);
	check_scalar_resumable(&training);
	check_scalar_reuse(&training);
	double error = 0., linear_error = 0.;
	for (unsigned i = TRAINING; i < SAMPLES; i++) {
		enum g711_lsb_mode operation = lsb[i % V92_OBSERVATION_PHASES];
		double predicted = v92_observation_predict_scalar_lsb(law, fitted, host_gain, u[i], offsets[i], operation);
		int16_t sample = (int16_t)fmax(INT16_MIN, fmin(INT16_MAX,
			round((fitted * u[i] - offsets[i]) / host_gain)));
		unsigned code = g711_encode(law, sample);
		if (operation != G711_LSB_UNCHANGED) code = (code & 254u) | (operation == G711_LSB_ONE);
		assert(predicted == host_gain * g711_decode(law, code) + offsets[i]);
		error += (predicted - y[i]) * (predicted - y[i]);
		linear_error += (u[i] - y[i]) * (u[i] - y[i]);
	}
	assert(error < linear_error / 4.);
	assert(!v92_observation_fit_scalar(NULL, &training));
	assert(!v92_observation_fit_scalar(&fitted, NULL));
	double before = fitted;
	for (unsigned fault = 0; fault < 17; fault++) {
		struct v92_observation_training bad = training;
		switch (fault) {
		case 0: bad.input = NULL; break;
		case 1: bad.linear = NULL; break;
		case 2: bad.sample = NULL; break;
		case 3: bad.levels = NULL; break;
		case 4: bad.count = 3; break;
		case 5: bad.count = TRAINING + 1; break;
		case 6: bad.law = (enum g711_law)-1; break;
		case 7: bad.host_gain = NAN; break;
		case 8: bad.host_gain = 0.; break;
		case 9: bad.host_gain = INFINITY; break;
		case 10: bad.input = (const int32_t *)&fitted; break;
		case 11: bad.linear = (const int16_t *)&fitted; break;
		case 12: bad.sample = (const int16_t *)&fitted; break;
		case 13: bad.levels = (const int16_t *)&fitted; break;
		case 14: bad.offsets = &fitted; break;
		case 15: bad.offsets = offsets; offsets[TRAINING - 1] = NAN; break;
		case 16: bad.offsets = offsets; offsets[TRAINING - 1] = INFINITY; break;
		}
		assert(!v92_observation_fit_scalar(&fitted, &bad) && fitted == before);
	}
	assert(!v92_observation_fit_scalar((double *)&training, &training));
	offsets[TRAINING - 1] = 0.;
	levels[TRAINING - 1] = 1; /* Not an actual codec level. */
	assert(!v92_observation_fit_scalar(&fitted, &training) && fitted == before);
	levels[TRAINING - 1] = quantize(law, 0);
	memset(u, 0, sizeof(u));
	assert(!v92_observation_fit_scalar(&fitted, &training) && fitted == before);
	assert(isnan(v92_observation_predict_scalar_lsb(law, 1., 1., 0, 0., (enum g711_lsb_mode)3)));
	assert(isnan(v92_observation_predict_scalar((enum g711_law)-1, 1., 1., 0, 0.)));
	assert(isnan(v92_observation_predict_scalar(law, NAN, 1., 0, 0.)));
	assert(isnan(v92_observation_predict_scalar(law, 0., 1., 0, 0.)));
	assert(isnan(v92_observation_predict_scalar(law, 1., DBL_TRUE_MIN, 32767, 0.)));
	assert(isnan(v92_observation_predict_scalar(law, DBL_MAX, 1., 32767, 0.)));
	assert(isnan(v92_observation_predict_scalar(law, 1., 1., 0, INFINITY)));
	for (int sign = -1; sign <= 1; sign += 2)
		assert(v92_observation_predict_scalar(law, 2., 1., sign * 32767, 0.) ==
			quantize(law, sign * 65534.));
	printf("V.92 scalar law=%u host=%.2f gain=%.6f heldout_rms=%.6f linear_rms=%.6f\n",
		law, host_gain, fitted, sqrt(error / (SAMPLES - TRAINING)),
		sqrt(linear_error / (SAMPLES - TRAINING)));
}

/* Exercise the public predictor across every integer and half-sample codec
 * boundary. Keep the independent round-and-clip oracle above: the model's
 * sample conversion must not depend on the host's floating rounding mode. */
static void check_scalar_rounding(void)
{
	const int modes[] = { FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO };
	int saved = fegetround();
	assert(saved != -1);
	for (unsigned mode = 0; mode < sizeof(modes) / sizeof(modes[0]); ++mode) {
		assert(fesetround(modes[mode]) == 0);
		for (int value = INT16_MIN; value <= INT16_MAX; ++value) {
			double half = value + .5;
			const double physical[] = { value, half,
				nextafter(half, -INFINITY), nextafter(half, INFINITY) };
			for (unsigned i = 0; i < sizeof(physical) / sizeof(physical[0]); ++i)
				for (unsigned law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law) {
					double offset = -physical[i];
					double expected = quantize(law, physical[i]) + offset;
					assert(v92_observation_predict_scalar(law, 1., 1., 0, offset) == expected);
				}
		}
	}
	assert(fesetround(saved) == 0);
}

/* Repeated and opposite coordinates make several codec transitions coincide.
 * Compare their JOINT gain integral with the independent encoder enumeration,
 * including A-law's zero/sign boundary and saturated int32 input coordinates.
 * A separate cache per tuple prevents a preceding score from standing in for
 * this observation. */
static void check_joint_transitions(const struct v92_observation *model)
{
	const int32_t values[] = { INT32_MIN, -65536, -32768, -4096, -128, -16, -4, -1,
		0, 1, 4, 16, 128, 4096, 32767, 65536, INT32_MAX };
	for (unsigned i = 0; i < sizeof(values) / sizeof(*values); i++)
		for (unsigned mixed = 0; mixed < 2; mixed++) {
			int32_t input[V92_OBSERVATION_TAPS];
			for (unsigned j = 0; j < model->taps; j++)
				input[j] = mixed && j % 2 ?
					(int32_t)fmax(INT32_MIN, fmin(INT32_MAX, -(double)values[i])) : values[i];
			double actual = v92_observation_predict(model, input, 0.);
			assert(isfinite(actual));
			int16_t observed = quantize(model->law, actual);
			struct v92_observation_cache cache;
			struct v92_observation_point history[V92_OBSERVATION_TAPS - 1];
			v92_observation_begin(&cache, model, 0.);
			assert(v92_observation_symbol_level(&cache, observed, &observed, 0.));
			for (unsigned j = 1; j < model->taps; j++)
				history[j - 1] = v92_observation_point(&cache, input[j]);
			uint64_t score = v92_observation_score(&cache, input[0], history, UINT64_MAX);
			double expected;
			uint64_t reference = independent_score(model, input, observed, observed, 0., &expected);
			assert(fabs(actual - expected) < 1e-7);
			/* The independent oracle floors probabilities at 1e-300;
			 * deep Gaussian tails are covered separately in check_tail_scores. */
			if (reference < (uint64_t)(-2e6 * log(1e-300)))
				assert(score <= reference + 1 && reference <= score + 1);
		}
}

int main(void)
{
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; law++)
		for (unsigned taps = 3; taps <= 4; taps++)
			for (enum g711_lsb_mode operation = G711_LSB_ZERO; operation <= G711_LSB_ONE; operation++)
				for (unsigned phase = 0; phase < V92_OBSERVATION_PHASES; phase++)
					check_periodic_training(law, taps, operation, phase);
	check_scalar_rounding();
	check_scalar_decline_reuse(G711_LAW_PCMU);
	check_scalar_decline_reuse(G711_LAW_PCMA);
	check_pruned_cost();
	check_coincident_fit(G711_LAW_PCMU);
	check_coincident_fit(G711_LAW_PCMA);
	const double gains[] = { .00002, .02, .125, 4., 16.39453125, 21.859375, 512. };
	struct v92_observation previous = { 0 };
	for (unsigned law = G711_LAW_PCMU; law <= G711_LAW_PCMA; law++)
		for (unsigned quarter = 3; quarter <= 5; quarter++)
			for (enum g711_lsb_mode mode = G711_LSB_UNCHANGED; mode <= G711_LSB_ONE; ++mode)
				check_scalar(law, quarter / 4., mode);
	for (unsigned law = G711_LAW_PCMU; law <= G711_LAW_PCMA; law++)
		for (unsigned quarter = 3; quarter <= 5; quarter++)
			check_conditioned_channel(law, quarter / 4.);
	for (unsigned law = G711_LAW_PCMU; law <= G711_LAW_PCMA; law++)
		for (unsigned i = 0; i < sizeof(gains) / sizeof(gains[0]); i++)
			check_channel(law, gains[i], 3, TRAINING, &previous);
	for (unsigned law = G711_LAW_PCMU; law <= G711_LAW_PCMA; law++) {
		check_channel(law, 4., 4, TRAINING, &previous);
		check_joint_transitions(&previous);
		check_extreme_bounds(&previous);
	}
	/* Short and odd acquisition spans exercise both finite-window edges. */
	const size_t counts[] = { TRAINING / 2, TRAINING - 3, TRAINING - 1 };
	for (unsigned law = G711_LAW_PCMU; law <= G711_LAW_PCMA; law++)
		for (unsigned taps = 3; taps <= 4; taps++)
			for (unsigned i = 0; i < sizeof(counts) / sizeof(*counts); i++)
				check_channel(law, 4., taps, counts[i], &previous);
	for (unsigned law = G711_LAW_PCMU; law <= G711_LAW_PCMA; law++)
		for (unsigned sigma = 16; sigma <= 64; sigma *= 2)
			check_noise_fit(law, sigma);
	/* Directed rounding uses the conditional-moment fallback. Exercise its
	 * delayed Gram rows, resumable ownership and held-out predictions too. */
	const int fit_modes[] = { FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO };
	int saved_rounding = fegetround();
	assert(saved_rounding != -1);
	for (unsigned mode = 0; mode < sizeof(fit_modes) / sizeof(*fit_modes); mode++) {
		assert(fesetround(fit_modes[mode]) == 0);
		for (unsigned law = G711_LAW_PCMU; law <= G711_LAW_PCMA; law++)
			for (unsigned taps = 3; taps <= 4; taps++)
				check_channel(law, 4., taps, TRAINING, &previous);
	}
	assert(fesetround(saved_rounding) == 0);
	struct v92_observation model, before;
	memset(&model, 0x5a, sizeof(model));
	before = model;
	int32_t zero[TRAINING] = { 0 };
	int16_t y[TRAINING] = { 0 };
	const struct v92_observation_training training = {
		.input = zero, .linear = y, .sample = y, .levels = y,
		.count = TRAINING, .taps = 3, .host_gain = 1., .law = G711_LAW_PCMU,
	};
	assert(!v92_observation_fit(&model, &training));
	assert(memcmp(&model, &before, sizeof(model)) == 0);
	for (unsigned law = 0; law < 2; law++)
		for (unsigned translated = 0; translated < 2; translated++) {
			check_scalar_codec_cells(law, translated);
			check_unit_cell_boundary();
			check_single_codec_cells(law, 1.25, translated);
			check_single_codec_cells(law, 1.9586004197173543, translated);
		}
	puts("V.92 observation checks passed");
	return 0;
}
