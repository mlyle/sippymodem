// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_ADAPTIVE_EQUALIZER_H
#define V34_ADAPTIVE_EQUALIZER_H

#include "v34_equalizer.h"
#include "v34_precoder.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V34_ADAPTIVE_EQUALIZER_QUALITY_SYMBOLS = 1024,
	V34_ADAPTIVE_EQUALIZER_RECENT_SYMBOLS = 512,
	V34_ADAPTIVE_EQUALIZER_QUALITY_BLOCK_SYMBOLS = 64,
	V34_ADAPTIVE_EQUALIZER_RECENT_BLOCKS =
		V34_ADAPTIVE_EQUALIZER_RECENT_SYMBOLS /
		V34_ADAPTIVE_EQUALIZER_QUALITY_BLOCK_SYMBOLS,
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
	/* Track carrier phase while preserving the trained FIR taps. */
	V34_ADAPTIVE_EQUALIZER_CARRIER,
	/* Track the center FIR span without rotating the carrier reference. */
	V34_ADAPTIVE_EQUALIZER_CENTER_TAPS,
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

/* Shared training adaptation schedule, using the valid line timing's nominal
 * symbol rate. The protocol owner decides when tracking starts or pauses;
 * deriving the power from its count avoids a second mutable training state. */
static inline uint16_t v34_training_decoder_power(
	uint16_t nominal_symbol_rate, uint32_t tracking_symbols)
{
	if (tracking_symbols >= nominal_symbol_rate)
		return 0x2000;
	if (tracking_symbols >= (uint16_t)(nominal_symbol_rate >> 1u))
		return 0x4000;
	return 0x7000;
}

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
				V34_ADAPTIVE_EQUALIZER_CARRIER ||
			action->equalizer.adaptation ==
				V34_ADAPTIVE_EQUALIZER_CENTER_TAPS ||
			action->equalizer.adaptation ==
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

/* The 1024-symbol online quality window is independent of the recent
 * 512-symbol training estimate, published every 64 committed decisions.
 * Both use the same full-window scale. Keeping short block sums avoids
 * either retaining acquisition transients or estimating from a tiny tail. */
struct v34_adaptive_equalizer_quality {
	uint32_t recent_error_blocks[V34_ADAPTIVE_EQUALIZER_RECENT_BLOCKS];
	uint32_t decision_accumulator;
	uint32_t feedback_accumulator;
	uint32_t reference_accumulator;
	uint32_t reference_metric;
	uint16_t window_position;
	uint16_t recent_symbol_count;
	int16_t recent_decision_metric;
	int16_t decision_metric;
	int16_t feedback_metric;
};

/* Exact FIR inputs and carrier frame for one observation whose semantic
 * target depends on the following observation.  V.34 data decisions are
 * four-dimensional, so the physical equalizer must retain this bounded
 * gradient until both two-dimensional symbols can be decided together. */
struct v34_adaptive_equalizer_retained {
	struct v34_adaptive_equalizer_observation observation;
	struct v34_analytic_sample demodulated;
	int16_t in_phase_delay[V34_EQUALIZER_MAX_TAPS];
	int16_t quadrature_delay[V34_EQUALIZER_MAX_TAPS];
	int16_t rotate_in_phase;
	int16_t rotate_quadrature;
	uint32_t equalizer_index;
	bool pending;
};

/* Own the fixed V.34 fractionally spaced equalizer and the decision feedback
 * that must remain continuous across Phase 3, Phase 4, B1, and data. Resolve
 * each observation before accepting more input, either by committing or
 * discarding it, or by retaining the first point for a joint 4-D decision. */
struct v34_adaptive_equalizer {
	struct v34_equalizer equalizer;
	struct v34_adaptive_equalizer_feedback feedback;
	struct v34_adaptive_equalizer_quality quality;
	struct v34_adaptive_equalizer_retained retained;
	struct v34_adaptive_equalizer_observation pending_observation;
	struct v34_analytic_sample pending_demodulated;
	int32_t phase_accumulator;
	int32_t phase_error;
	int16_t rotate_in_phase;
	int16_t rotate_quadrature;
	int16_t pending_rotate_in_phase;
	int16_t pending_rotate_quadrature;
	uint16_t carrier_phase_low;
	uint16_t carrier_phase_fraction;
	uint64_t epoch;
	uint64_t lineage;
	uint8_t inputs_since_observation;
	uint8_t feedback_mode;
	bool data_carrier_tracking;
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

/* Freeze the frequency estimate learned from known training symbols while
 * retaining bounded, low-gain phase tracking for tentative data decisions.
 * Disabling the policy restores the ordinary acquisition loop without
 * changing any learned carrier state.  Transitions occur only at a complete
 * semantic observation boundary. */
enum v34_adaptive_equalizer_result
v34_adaptive_equalizer_set_data_carrier_tracking(
	struct v34_adaptive_equalizer *equalizer, bool data_tracking);

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

/* Retain the pending observation while admitting exactly one following
 * observation.  This is available only without decision feedback, whose
 * mutable history cannot be replayed after a delayed decision. */
enum v34_adaptive_equalizer_result
v34_adaptive_equalizer_retain_observation(
	struct v34_adaptive_equalizer *equalizer,
	const struct v34_adaptive_equalizer_observation *observation);

/* Resolve the retained and current observations atomically and in signal
 * order.  Both decisions use the FIR inputs and carrier frame captured when
 * their observations were produced. */
enum v34_adaptive_equalizer_result v34_adaptive_equalizer_commit_pair(
	struct v34_adaptive_equalizer *equalizer,
	const struct v34_adaptive_equalizer_decision decisions[2]);

/* Commit the semantic target. SKIP retains the V.34 data-demapper behavior:
 * decision feedback advances, while carrier and FIR adaptation do not.
 * CARRIER updates only carrier phase; CENTER_TAPS updates the center FIR span
 * without moving the carrier reference; CENTER and FULL update both. */
enum v34_adaptive_equalizer_result v34_adaptive_equalizer_commit(
	struct v34_adaptive_equalizer *equalizer,
	const struct v34_adaptive_equalizer_decision *decision);

/* Consume an interval with no meaningful target. Learning alone has not yet
 * mutated feedback history, so only an observation that applied the filter is
 * nondiscardable. */
enum v34_adaptive_equalizer_result v34_adaptive_equalizer_discard(
	struct v34_adaptive_equalizer *equalizer,
	const struct v34_adaptive_equalizer_observation *observation);

/* Discard an erased observation and advance carrier phase at the retained
 * frequency, without learning from its target. Call at the same cadence as
 * ordinary carrier adaptation; use discard for intervening SKIP symbols. */
enum v34_adaptive_equalizer_result v34_adaptive_equalizer_coast(
	struct v34_adaptive_equalizer *equalizer,
	const struct v34_adaptive_equalizer_observation *observation);

#endif /* V34_ADAPTIVE_EQUALIZER_H */
