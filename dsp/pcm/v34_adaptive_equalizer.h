// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_ADAPTIVE_EQUALIZER_H
#define V34_ADAPTIVE_EQUALIZER_H

#include "v34_equalizer.h"
#include "v34_precoder.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V34_ADAPTIVE_EQUALIZER_QUALITY_SYMBOLS = 1024,
	V34_ADAPTIVE_EQUALIZER_INPUTS_PER_OBSERVATION = 2,
	/* The identity tap first reaches a complete semantic observation at
	 * this epoch after a fresh delay line. */
	V34_ADAPTIVE_EQUALIZER_FIRST_SIGNAL_EPOCH =
		V34_EQUALIZER_IDENTITY_TAP /
		V34_ADAPTIVE_EQUALIZER_INPUTS_PER_OBSERVATION,
};

_Static_assert(
	V34_EQUALIZER_IDENTITY_TAP %
		V34_ADAPTIVE_EQUALIZER_INPUTS_PER_OBSERVATION ==
		0,
	"V.34 equalizer identity tap must end on an observation boundary");

enum v34_adaptive_equalizer_adaptation {
	V34_ADAPTIVE_EQUALIZER_SKIP,
	V34_ADAPTIVE_EQUALIZER_CENTER,
	V34_ADAPTIVE_EQUALIZER_FULL,
};

enum v34_adaptive_equalizer_feedback_mode {
	V34_ADAPTIVE_EQUALIZER_FEEDBACK_NONE = 0,
	/* Learn the three-tap coefficients from semantic decisions. */
	V34_ADAPTIVE_EQUALIZER_FEEDBACK_LEARN = 1u << 0,
	/* Apply the learned filter before publishing a semantic observation. */
	V34_ADAPTIVE_EQUALIZER_FEEDBACK_APPLY = 1u << 1,
};

enum v34_adaptive_equalizer_result {
	V34_ADAPTIVE_EQUALIZER_OK,
	V34_ADAPTIVE_EQUALIZER_INVALID_ARGUMENT,
	V34_ADAPTIVE_EQUALIZER_INVALID_STATE,
};

struct v34_adaptive_equalizer_observation {
	struct v34_analytic_sample decision;
	uint64_t epoch;
	uint64_t lineage;
};

struct v34_adaptive_equalizer_decision {
	struct v34_adaptive_equalizer_observation observation;
	struct v34_analytic_sample target;
	uint16_t decoder_power_q15;
	enum v34_adaptive_equalizer_adaptation adaptation;
};

/* Every semantic receiver that consumes an observation returns exactly one
 * operation for the persistent physical receive owner. */
enum v34_receive_action_kind {
	V34_RECEIVE_COMMIT,
	V34_RECEIVE_DISCARD,
	/* Discard the boundary decision, then start full tracking. */
	V34_RECEIVE_BEGIN_TRACKING,
	/* Discard the boundary decision, then restart timing and equalization. */
	V34_RECEIVE_RESTART_PATH,
};

struct v34_receive_action {
	struct v34_adaptive_equalizer_decision equalizer;
	enum v34_receive_action_kind action;
};

static inline struct v34_receive_action v34_receive_action_only(
	enum v34_receive_action_kind action)
{
	return (struct v34_receive_action){ .action = action };
}

static inline struct v34_receive_action v34_receive_action_commit(
	const struct v34_adaptive_equalizer_observation *observation,
	struct v34_analytic_sample target, uint16_t decoder_power_q15,
	enum v34_adaptive_equalizer_adaptation adaptation)
{
	return (struct v34_receive_action){
		.equalizer = {
			.observation = *observation,
			.target = target,
			.decoder_power_q15 = decoder_power_q15,
			.adaptation = adaptation,
		},
		.action = V34_RECEIVE_COMMIT,
	};
}

static inline bool v34_adaptive_equalizer_observation_is_zero(
	const struct v34_adaptive_equalizer_observation *observation)
{
	return observation->decision.in_phase == 0 &&
	       observation->decision.quadrature == 0 &&
	       observation->epoch == 0u && observation->lineage == 0u;
}

static inline bool v34_adaptive_equalizer_observations_are_equal(
	const struct v34_adaptive_equalizer_observation *left,
	const struct v34_adaptive_equalizer_observation *right)
{
	return left->decision.in_phase == right->decision.in_phase &&
	       left->decision.quadrature == right->decision.quadrature &&
	       left->epoch == right->epoch && left->lineage == right->lineage;
}

static inline bool v34_adaptive_equalizer_observation_is_valid(
	const struct v34_adaptive_equalizer_observation *observation)
{
	return observation != NULL && observation->epoch != 0u &&
	       observation->lineage != 0u;
}

static inline bool v34_receive_action_is_valid(
	const struct v34_receive_action *action,
	const struct v34_adaptive_equalizer_observation *observation,
	uint16_t decoder_power_q15)
{
	if (action == NULL ||
	    !v34_adaptive_equalizer_observation_is_valid(observation) ||
	    decoder_power_q15 == 0u || decoder_power_q15 > INT16_MAX)
		return false;
	if (action->action == V34_RECEIVE_COMMIT)
		return v34_adaptive_equalizer_observations_are_equal(
			       &action->equalizer.observation, observation) &&
		       action->equalizer.decoder_power_q15 == decoder_power_q15 &&
		       (action->equalizer.adaptation ==
				V34_ADAPTIVE_EQUALIZER_CENTER ||
			action->equalizer.adaptation ==
				V34_ADAPTIVE_EQUALIZER_FULL);
	return (action->action == V34_RECEIVE_DISCARD ||
		action->action == V34_RECEIVE_BEGIN_TRACKING ||
		action->action == V34_RECEIVE_RESTART_PATH) &&
	       v34_adaptive_equalizer_observation_is_zero(
		       &action->equalizer.observation) &&
	       action->equalizer.target.in_phase == 0 &&
	       action->equalizer.target.quadrature == 0 &&
	       action->equalizer.decoder_power_q15 == 0u &&
	       action->equalizer.adaptation == V34_ADAPTIVE_EQUALIZER_SKIP;
}

struct v34_adaptive_equalizer_feedback {
	int16_t coefficient_a[3];
	int16_t coefficient_b[3];
	int16_t in_phase_history[4];
	int16_t quadrature_history[4];
};

/* One 1024-symbol decision-quality window.  The published metrics remain
 * available while the next window is accumulated. */
struct v34_adaptive_equalizer_quality {
	uint32_t decision_accumulator;
	uint32_t feedback_accumulator;
	uint32_t reference_accumulator;
	uint32_t reference_metric;
	uint16_t window_position;
	int16_t decision_metric;
	int16_t feedback_metric;
};

/* Own the fixed V.34 fractionally spaced equalizer and the decision feedback
 * that must remain continuous across Phase 3, Phase 4, B1, and data.  One
 * observation is completed or discarded before another input is accepted. */
struct v34_adaptive_equalizer {
	struct v34_equalizer equalizer;
	struct v34_adaptive_equalizer_feedback feedback;
	struct v34_adaptive_equalizer_quality quality;
	struct v34_adaptive_equalizer_observation pending_observation;
	struct v34_analytic_sample pending_demodulated;
	int32_t phase_accumulator;
	int32_t phase_error;
	int16_t rotate_in_phase;
	int16_t rotate_quadrature;
	uint16_t carrier_phase_low;
	uint16_t carrier_phase_fraction;
	uint64_t epoch;
	uint64_t lineage;
	uint8_t inputs_since_observation;
	uint8_t feedback_mode;
	bool decision_pending;
	bool initialized;
};

enum v34_adaptive_equalizer_result v34_adaptive_equalizer_init(
	struct v34_adaptive_equalizer *equalizer);
bool v34_adaptive_equalizer_is_valid(
	const struct v34_adaptive_equalizer *equalizer);

/* Select feedback learning and application independently at a decision
 * boundary. Phase 3 learns without applying the filter; negotiated data may
 * apply the frozen result without continuing to learn. Coefficients and
 * histories are retained across mode changes. */
enum v34_adaptive_equalizer_result
v34_adaptive_equalizer_set_feedback_mode(
	struct v34_adaptive_equalizer *equalizer,
	enum v34_adaptive_equalizer_feedback_mode mode);

/* Freeze the learned decision-feedback filter at the local MP boundary and
 * return its three transmitter-precoder coefficients in h(1)..h(3) order.
 * The filter history is cleared at the B1 zero-history boundary. The
 * partially accumulated equalizer input, FIR, carrier, quality, and learned
 * coefficients remain continuous. */
enum v34_adaptive_equalizer_result
v34_adaptive_equalizer_finish_feedback_training(
	struct v34_adaptive_equalizer *equalizer,
	struct v34_q14_coefficient
		coefficients[V34_PRECODER_TAP_COUNT]);

/* Begin full equalizer tracking at the qualified S/S-bar boundary.  Clear
 * the eight center coefficients, decision-feedback coefficients, and the old
 * decision-quality window.  Residues, delayed inputs, feedback histories,
 * and carrier state remain continuous. */
enum v34_adaptive_equalizer_result
v34_adaptive_equalizer_begin_tracking(
	struct v34_adaptive_equalizer *equalizer);

/* Supply one of the two recovered complex samples per V.34 symbol. */
enum v34_adaptive_equalizer_result v34_adaptive_equalizer_push(
	struct v34_adaptive_equalizer *equalizer,
	struct v34_analytic_sample sample);

/* Filter one symbol, apply the retained decision-feedback and carrier
 * correction, and publish the exact fixed-coordinate semantic observation. */
enum v34_adaptive_equalizer_result v34_adaptive_equalizer_observe(
	struct v34_adaptive_equalizer *equalizer,
	struct v34_adaptive_equalizer_observation *observation);

/* Commit the semantic target.  SKIP retains the V.34 data-demapper
 * behavior: decision feedback advances, while carrier and FIR adaptation do
 * not.  CENTER and FULL update the corresponding FIR span. */
enum v34_adaptive_equalizer_result v34_adaptive_equalizer_commit(
	struct v34_adaptive_equalizer *equalizer,
	const struct v34_adaptive_equalizer_decision *decision);

/* Consume an interval with no meaningful target. Learning alone has not yet
 * mutated feedback history, so only an observation that applied the filter is
 * nondiscardable. */
enum v34_adaptive_equalizer_result v34_adaptive_equalizer_discard(
	struct v34_adaptive_equalizer *equalizer,
	const struct v34_adaptive_equalizer_observation *observation);

#endif /* V34_ADAPTIVE_EQUALIZER_H */
