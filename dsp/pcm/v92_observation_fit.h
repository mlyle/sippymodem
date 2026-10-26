// SPDX-License-Identifier: BSD-3-Clause
#ifndef V92_OBSERVATION_FIT_H
#define V92_OBSERVATION_FIT_H

#include "v92_observation.h"
#include "gram_fit.h"

struct v92_observation_fit_interval {
	double low, high, coefficient[V92_OBSERVATION_TAPS], error, weight;
};
struct v92_observation_fit_moments {
	double gram[GRAM_FIT_MOMENTS], rhs[V92_OBSERVATION_TAPS], energy;
};
struct v92_observation_fit_boundary { double at; unsigned next; };

enum v92_observation_fit_stage {
	V92_FIT_IDLE, V92_FIT_SWEEP, V92_FIT_CENTER, V92_FIT_BOUNDS,
	V92_FIT_REFINE, V92_FIT_WEIGHTS, V92_FIT_GEOMETRY, V92_FIT_VALIDATE,
};

/* Receiver-owned fitting scratch. No borrowed pointer survives a call.
 * Keep the validated training, its arrays and the floating-point rounding
 * mode unchanged until completion.
 * A suspended fit can be discarded or restarted without publishing a model. */
struct v92_observation_fit_work {
	struct v92_observation model;
	struct v92_observation_fit_interval fits[V92_OBSERVATION_INTERVALS];
	struct v92_observation_fit_boundary events[V92_OBSERVATION_TRAINING];
	struct v92_observation_fit_moments moments;
	size_t heap[V92_OBSERVATION_TRAINING], used;
	int16_t q[V92_OBSERVATION_TRAINING];
	double low, end, linear_error;
	bool pending;
	double lower[V92_OBSERVATION_TRAINING], upper[V92_OBSERVATION_TRAINING];
	double costs[V92_OBSERVATION_INTERVALS], variances[V92_OBSERVATION_INTERVALS];
	int16_t bins[V92_OBSERVATION_INTERVALS][V92_OBSERVATION_TRAINING];
	enum v92_observation_fit_stage stage;
	unsigned cursor;
};

enum v92_observation_fit_result {
	V92_OBSERVATION_FIT_MORE, V92_OBSERVATION_FIT_READY, V92_OBSERVATION_FIT_DECLINED,
};

/* Internal exclusively owned continuation. Begin validates the same training
 * contract as fit(); scratch may not overlap training or its arrays. False
 * means no fit. Call step only after successful begin, using that immutable
 * training and an output disjoint from both scratch and training.
 *
 * Each step handles at most 1024 gain-cell solves/codec transitions, one
 * retained channel refinement (including its existing bounded fallback), or
 * one final admission stage. MORE and DECLINED leave output unchanged. READY
 * publishes the complete model and ends the job. No partially fitted model
 * is usable for decoding or B1 admission. */
bool v92_observation_fit_begin(struct v92_observation_fit_work *work,
	const struct v92_observation_training *training);
enum v92_observation_fit_result v92_observation_fit_step(struct v92_observation_fit_work *work,
	const struct v92_observation_training *training, struct v92_observation *output);

#endif /* V92_OBSERVATION_FIT_H */
