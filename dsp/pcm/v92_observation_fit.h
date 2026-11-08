// SPDX-License-Identifier: BSD-3-Clause
#ifndef V92_OBSERVATION_FIT_H
#define V92_OBSERVATION_FIT_H

#include "v92_observation.h"
#include "gram_fit.h"

#include <fenv.h>
#include <float.h>

struct v92_observation_fit_interval {
	double low, high, coefficient[V92_OBSERVATION_TAPS], error, weight;
};
struct v92_observation_fit_moments {
	double gram[GRAM_FIT_MOMENTS], rhs[V92_OBSERVATION_TAPS], energy;
};
struct v92_observation_fit_boundary { double at; unsigned next; };

enum v92_observation_fit_stage {
	V92_FIT_IDLE, V92_FIT_SCALAR, V92_FIT_SWEEP, V92_FIT_CENTER,
	V92_FIT_REFINE, V92_FIT_WEIGHTS, V92_FIT_GEOMETRY, V92_FIT_VALIDATE,
};

enum { V92_SCALAR_FIT_UPDATES = 96, V92_SCALAR_FIT_BATCH = 12,
	V92_SCALAR_FIT_STEPS = V92_SCALAR_FIT_UPDATES / V92_SCALAR_FIT_BATCH };

enum v92_observation_fit_result {
	V92_OBSERVATION_FIT_MORE, V92_OBSERVATION_FIT_READY, V92_OBSERVATION_FIT_DECLINED,
};

/* Explicit completion from the scalar solver. MORE is unavailable, not a
 * declined fit. READY alone carries a positive finite gain. */
struct v92_scalar_fit_result {
	double gain;
	enum v92_observation_fit_result status;
	int rounding;
};

static inline bool v92_scalar_fit_result_is_valid(struct v92_scalar_fit_result r)
{
	return r.status == V92_OBSERVATION_FIT_READY ?
		r.gain > 0. && r.gain <= DBL_MAX :
		(r.status == V92_OBSERVATION_FIT_MORE ||
		 r.status == V92_OBSERVATION_FIT_DECLINED) && r.gain == 0.;
}

/* A completed value is reusable only in its original numerical context.
 * An unavailable rounding-mode query retains ordinary, uncached fitting. */
static inline bool v92_scalar_fit_result_can_reuse(struct v92_scalar_fit_result r)
{
	return v92_scalar_fit_result_is_valid(r) &&
	       r.status != V92_OBSERVATION_FIT_MORE &&
	       r.rounding >= 0 && r.rounding == fegetround();
}

struct v92_scalar_fit {
	double gain, variance, linear_error, xx;
	unsigned iterations;
};

/* Receiver-owned fitting scratch. No borrowed pointer survives a call.
 * Keep the validated training, its arrays and the floating-point rounding
 * mode unchanged until completion.
 * A suspended fit can be discarded or restarted without publishing a model. */
struct v92_observation_fit_work {
	struct v92_observation model;
	/* Scalar admission precedes gain-cell fitting; the owners are exclusive. */
	union {
		struct v92_observation_fit_interval fits[V92_OBSERVATION_INTERVALS];
		struct { struct v92_scalar_fit fit; double low, high; } scalar;
	};
	struct v92_observation_fit_boundary events[V92_OBSERVATION_TRAINING];
	struct v92_observation_fit_moments moments;
	size_t heap[V92_OBSERVATION_TRAINING], used;
	int16_t q[V92_OBSERVATION_TRAINING];
	double low, end, linear_error;
	bool pending;
	/* Host-coordinate codec cells, prepared once before fitting. */
	double lower[V92_OBSERVATION_TRAINING], upper[V92_OBSERVATION_TRAINING];
	double costs[V92_OBSERVATION_INTERVALS], variances[V92_OBSERVATION_INTERVALS];
	int16_t bins[V92_OBSERVATION_INTERVALS][V92_OBSERVATION_TRAINING];
	/* Exact sum from completed final admission; only a current READY grants
	 * access. UINT64_MAX means unavailable: missing/unscorable wire evidence,
	 * pruned/declined fits and scalar-only jobs publish no cost.
	 * Invalid begin grants no new result and can preserve the previous job. */
	uint64_t training_cost;
	/* Completed scalar work survives a later tandem-fit decline. A generic
	 * decline cannot supply this result. Begin invalidates it as above. */
	struct v92_scalar_fit_result scalar_result;
	enum v92_observation_fit_stage stage;
	unsigned cursor;
};

/* Internal exclusively owned continuation. Begin validates the same training
 * contract as fit(); scratch may not overlap training or its arrays. False
 * means no fit. Call step only after successful begin, using that immutable
 * training and an output disjoint from both scratch and training.
 *
 * Each step handles at most twelve scalar updates, 1024 gain-cell solves/codec transitions, one
 * retained channel refinement (including its existing bounded fallback), or
 * one final admission stage. MORE and DECLINED leave output unchanged. READY
 * publishes the complete model and ends the job. No partially fitted model
 * is usable for decoding or B1 admission. */
bool v92_observation_fit_begin(struct v92_observation_fit_work *work,
	const struct v92_observation_training *training);
enum v92_observation_fit_result v92_observation_fit_step(struct v92_observation_fit_work *work,
	const struct v92_observation_training *training, struct v92_observation *output);

/* Internal continuation for a validated job at the scalar stage. The completed
 * result must belong to identical immutable linear/sample/wire/offset
 * coordinates, count, taps, law, host gain, final-codec operations/physical
 * phase and rounding mode. A null-codec result cannot cross a changed
 * operation pattern. Input may
 * differ. Unavailable/malformed results return DECLINED without changing the
 * job. Valid completion returns MORE to start its gain sweep, or DECLINED
 * to end the fit when the scalar exactly explains the cells. This never
 * publishes a model or retains a pointer. */
enum v92_observation_fit_result v92_observation_fit_finish_scalar_owned(
	struct v92_observation_fit_work *work,
	const struct v92_observation_training *training,
	struct v92_scalar_fit_result result);

/* Same immutable-training and exclusive-scratch contract, for the scalar
 * fallback alone. Actual wire levels are required. MORE and DECLINED leave
 * gain unchanged; READY publishes the same completed fit as fit_scalar().
 * A terminal step also retains its explicit scalar_result. */
bool v92_observation_scalar_begin(struct v92_observation_fit_work *work,
	const struct v92_observation_training *training);
enum v92_observation_fit_result v92_observation_scalar_step(struct v92_observation_fit_work *work,
	const struct v92_observation_training *training, double *gain);

#endif /* V92_OBSERVATION_FIT_H */
