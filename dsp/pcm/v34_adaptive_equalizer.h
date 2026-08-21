// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_ADAPTIVE_EQUALIZER_H
#define V34_ADAPTIVE_EQUALIZER_H

#include "v34_equalizer.h"
#include "v34_precoder.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V34_ADAPTIVE_EQUALIZER_QUALITY_SYMBOLS = 1024,
};

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
