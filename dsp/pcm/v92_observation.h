// SPDX-License-Identifier: BSD-3-Clause
#ifndef V92_OBSERVATION_H
#define V92_OBSERVATION_H

#include "g711.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	V92_OBSERVATION_TRAINING = 24 * 12,
	V92_OBSERVATION_INTERVALS = 8,
	V92_OBSERVATION_STEPS = 2 * V92_OBSERVATION_INTERVALS * 256 + 4,
	V92_OBSERVATION_BUCKETS = 4096,
	V92_OBSERVATION_CACHE = 2048,
	V92_OBSERVATION_POINTS = 1024,
};

/* A log-uniform first-codec gain interval and its fitted three channel taps.
 * Training retains the eight lowest-error identifiable intervals. */
struct v92_observation_interval {
	double low, high, main, post, second, weight;
};
/* Equal endpoints are constant. A zero crossing with unequal endpoints
 * denotes multiple transitions, handled by the general codec-cell integral. */
struct v92_observation_cell {
	int16_t low, high;
	uint16_t crossing;
};
struct v92_observation_values {
	double main_low, main_high, post_low, post_high, second_low, second_high;
	struct v92_observation_cell cell[V92_OBSERVATION_INTERVALS];
	bool stable;
};

/* Owned by the receiver, immutable throughout one mapping/calibration epoch.
 * Do not copy this table into competing traceback paths. */
struct v92_observation {
	int32_t first[V92_OBSERVATION_STEPS];
	struct v92_observation_interval intervals[V92_OBSERVATION_INTERVALS];
	uint16_t bucket[V92_OBSERVATION_BUCKETS + 1];
	/* quantizer_gain is the positive host gain after the final codec;
	 * zero selects point observations during the initial center fit. */
	double variance, energy, quantizer_gain;
	size_t count;
	enum g711_law law;
	/* Zero means identity; otherwise apply the known one-tap CPd prefilter
	 * rounding before the unknown first-codec gain. Delay is line-owned. */
	int16_t input_coefficient;
};

/* Fit at most the first 24 known B1u frames. x is the signal before the first
 * codec; y is the received host sample; u is the existing linear prediction.
 * Reject a fit that does not improve on u. This is not an arbitrary channel:
 * the supported model is Q(g*x), three channel taps, then a final codec.
 *
 * A rejected fit leaves output unchanged. At a new training epoch the caller
 * must invalidate its previous model before collecting acquisition decisions.
 * Fitting is not B1 admission: held-out decoded bits and line quality must pass. */
bool v92_observation_fit(struct v92_observation *model, enum g711_law law,
	const int32_t *x, const int16_t *u, const int16_t *y, size_t count);
bool v92_observation_is_valid(const struct v92_observation *model);
/* After setting the positive final-codec host quantizer_gain, jointly refine
 * channel taps and noise variance against the observed codec intervals.
 * levels optionally supplies aligned decoded wire levels (before host gain),
 * avoiding inference from y.
 * x must use the same coordinates as the initial fit. Set input_coefficient
 * only after fitting/refitting already-rounded training inputs. Neither fit
 * may overlap its output with any input; rejection leaves output unchanged. */
bool v92_observation_refit_bins(struct v92_observation *model,
	const int32_t *x, const int16_t *y, const int16_t *levels, size_t count);

struct v92_observation_point {
	struct v92_observation_values values;
	int32_t x, key;
	size_t span;
};
/* Frame-local scratch, borrowing the validated immutable model. Its symbols
 * may have different observed samples and noise; scores cannot cross symbols. */
struct v92_observation_cache {
	uint64_t key[V92_OBSERVATION_CACHE], value[V92_OBSERVATION_CACHE];
	int32_t second_key[V92_OBSERVATION_CACHE];
	uint8_t stamp[V92_OBSERVATION_CACHE], generation;
	struct v92_observation_values points[V92_OBSERVATION_POINTS];
	uint16_t point_tag[V92_OBSERVATION_POINTS];
	const struct v92_observation *model;
	double relative_noise, variance, bound_scale, lower, upper;
	/* Immutable per-fit constants and the current symbol's Gaussian scale. */
	double width[V92_OBSERVATION_INTERVALS], margin[3], divisor;
	int16_t observed;
};

/* Fast paths require nonoverlapping scratch and a validated immutable model.
 * Begin resets all cache stamps. Symbol also handles generation wrap. A
 * rejected symbol setup leaves scratch unchanged. A successful setup is
 * required before scoring; points remain usable across symbols of the same
 * model, but not across model changes. */
void v92_observation_begin(struct v92_observation_cache *cache,
	const struct v92_observation *model, double relative_noise);
bool v92_observation_symbol(struct v92_observation_cache *cache, int16_t observed);
bool v92_observation_symbol_level(struct v92_observation_cache *cache,
	int16_t observed, const int16_t *level);
struct v92_observation_point v92_observation_point(
	struct v92_observation_cache *cache, int32_t x);
/* Exact below budget; may return a conservative lower bound >= budget when
 * the candidate cannot win. UINT64_MAX requests an unbounded exact score. */
uint64_t v92_observation_score(struct v92_observation_cache *cache, int32_t x,
	const struct v92_observation_point *previous,
	const struct v92_observation_point *second, uint64_t budget);

struct v92_observation_range { double low, high; };
struct v92_observation_range v92_observation_range(
	struct v92_observation_cache *cache, int32_t x);

/* Predict the final reconstructed sample for a validated fitted model with
 * positive quantizer_gain. Integrate the same shared gain across current,
 * previous and second-previous input, without conditioning on the observed
 * sample. Non-finite output means no prediction, never a zero residual. */
double v92_observation_predict(const struct v92_observation *model,
	const int32_t input[3]);

#endif /* V92_OBSERVATION_H */
