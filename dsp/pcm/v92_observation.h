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
	V92_OBSERVATION_SPANS = 1024,
	V92_OBSERVATION_TAPS = 4,
};

/* A log-uniform first-codec gain interval and its fitted channel taps.
 * Training retains the eight lowest-error identifiable intervals. */
struct v92_observation_interval {
	double low, high, coefficient[V92_OBSERVATION_TAPS], weight;
};
/* Equal endpoints are constant. A zero crossing with unequal endpoints
 * denotes multiple transitions, handled by the general codec-cell integral. */
struct v92_observation_cell {
	int16_t low, high;
	uint16_t crossing;
};
struct v92_observation_values {
	double low[V92_OBSERVATION_TAPS], high[V92_OBSERVATION_TAPS];
	struct v92_observation_cell cell[V92_OBSERVATION_INTERVALS];
	bool stable;
};

/* Owned by the receiver, immutable throughout one mapping/calibration epoch.
 * Do not copy this table into competing traceback paths. */
struct v92_observation {
	int32_t first[V92_OBSERVATION_STEPS];
	struct v92_observation_interval intervals[V92_OBSERVATION_INTERVALS];
	uint16_t bucket[V92_OBSERVATION_BUCKETS + 1];
	/* Positive host gain after the final codec. */
	double variance, energy, quantizer_gain;
	size_t count;
	unsigned taps;
	enum g711_law law;
	/* Zero means identity; otherwise apply the known one-tap CPd prefilter
	 * rounding before the unknown first-codec gain. Delay is line-owned. */
	int16_t input_coefficient;
};

/* Borrowed, aligned samples from at most the first 24 known B1u frames.
 * input precedes the first codec; sample follows the final codec and positive
 * host gain. levels optionally supplies decoded wire levels before host gain,
 * avoiding inference from sample. linear is the existing linear prediction. */
struct v92_observation_training {
	const int32_t *input;
	const int16_t *linear, *sample, *levels;
	/* Optional additive host corrections after the final codec and gain,
	 * in sample coordinates (e.g. independently estimated echo subtraction).
	 * For y = gain * level + offset, translate the wire cell by offset and
	 * predict gain * Q((linear - offset) / gain) + offset in the same units.
	 * Requires levels. No host clipping may follow the affine correction;
	 * its provenance/validity belongs to the caller, never a decoded candidate. */
	const double *offsets;
	size_t count;
	/* Fit three or four causal channel coefficients. */
	unsigned taps;
	double host_gain;
	enum g711_law law;
};

/* Fit Q(g*input), the selected channel taps and noise, then a final codec.
 * The center fit must improve on linear before codec-bin refinement. With
 * actual wire levels, decline fitting if the unchanged linear prediction
 * already lies strictly inside every observed cell, or the existing scalar
 * fitter returns a gain strictly inside their common intersection and
 * reconstructs every scored wire level. Otherwise the final model must
 * also improve observed-codec likelihood over linear under the
 * same fitted noise variance. Compile only the final model: no partially
 * fitted model is exposed to the caller.
 *
 * Output may not overlap training or any of its arrays. Rejection leaves it
 * unchanged. Set input_coefficient only after fitting already-rounded inputs.
 * At a new training epoch, invalidate the previous model before collecting
 * acquisition decisions.
 * Fitting is not B1 admission: held-out decoded bits and line quality must pass. */
bool v92_observation_fit(struct v92_observation *model,
	const struct v92_observation_training *training);
bool v92_observation_is_valid(const struct v92_observation *model);

/* Optional single-codec prediction when the tandem model cannot explain a
 * channel. Fit a positive gain on training.linear using the actual final
 * wire levels and independent host corrections. The shared training/alias
 * contracts apply; levels is required. Commit only if reconstruction improves
 * on linear, leaving gain unchanged on rejection. This is not B1 admission:
 * freeze the result before held-out bits and line-quality validation.
 * predict_scalar takes a linear constellation point, never the observed value;
 * host_gain and offset describe the same final-codec affine transform as above.
 * It returns NAN for invalid parameters or unrepresentable arithmetic. */
bool v92_observation_fit_scalar(double *gain,
	const struct v92_observation_training *training);
double v92_observation_predict_scalar(enum g711_law law, double gain,
	double host_gain, int16_t linear, double offset);

struct v92_observation_point {
	struct v92_observation_values values;
	int32_t x, key;
	size_t span;
};
/* Frame-local scratch, borrowing the validated immutable model. Its symbols
 * may have different observed samples and noise; scores cannot cross symbols. */
struct v92_observation_cache {
	/* Derived once at begin(), like the other immutable per-fit constants. */
	unsigned bucket_shift;
	int64_t bucket_extent;
	uint64_t value[V92_OBSERVATION_CACHE];
	int32_t key[V92_OBSERVATION_CACHE][V92_OBSERVATION_TAPS];
	uint8_t stamp[V92_OBSERVATION_CACHE], generation;
	struct v92_observation_values points[V92_OBSERVATION_POINTS];
	uint16_t point_tag[V92_OBSERVATION_POINTS];
	/* Coordinate-to-span memoization is model-only, shared across symbols.
	 * Zero tags are empty; begin() invalidates every entry. */
	struct { int32_t key; uint16_t tag; } spans[V92_OBSERVATION_SPANS];
	const struct v92_observation *model;
	double relative_noise, variance, bound_scale, lower, upper;
	/* Immutable per-fit constants and the current symbol's Gaussian scale. */
	double width[V92_OBSERVATION_INTERVALS], log_weight[V92_OBSERVATION_INTERVALS];
	double margin[V92_OBSERVATION_TAPS], divisor;
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
/* A nonzero offset requires the actual wire level; no requantization of
 * the conditioned sample may stand in for missing wire evidence. */
bool v92_observation_symbol_level(struct v92_observation_cache *cache,
	int16_t observed, const int16_t *level, double offset);
struct v92_observation_point v92_observation_point(
	struct v92_observation_cache *cache, int32_t x);
/* Exact below budget; may return a conservative lower bound >= budget when
 * the candidate cannot win. UINT64_MAX requests an unbounded exact score.
 * previous contains model->taps - 1 points, newest first. */
uint64_t v92_observation_score(struct v92_observation_cache *cache, int32_t x,
	const struct v92_observation_point *previous, uint64_t budget);

struct v92_observation_range { double low, high; };
struct v92_observation_range v92_observation_range(
	struct v92_observation_cache *cache, int32_t x);

/* Predict the mean final reconstructed sample for a validated fitted model
 * with positive quantizer_gain. Integrate fitted Gaussian channel noise and
 * the same shared gain across model->taps current and preceding inputs,
 * without conditioning on the observed sample. offset is the same independent
 * additive host correction supplied to symbol_level(), not a residual fitted
 * to the winning candidate.
 * Non-finite output means no prediction, never a zero residual. */
double v92_observation_predict(const struct v92_observation *model,
	const int32_t *input, double offset);

#endif /* V92_OBSERVATION_H */
