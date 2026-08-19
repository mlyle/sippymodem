// SPDX-License-Identifier: BSD-3-Clause

#ifndef V32_EQUALIZER_H
#define V32_EQUALIZER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	V32_EQUALIZER_TAPS = 103,
};

enum v32_equalizer_result {
	V32_EQUALIZER_OK,
	V32_EQUALIZER_OBSERVATION_READY,
	V32_EQUALIZER_DECISION_PENDING,
	V32_EQUALIZER_INVALID_ARGUMENT,
	V32_EQUALIZER_INVALID_STATE,
};

enum v32_equalizer_adaptation {
	V32_EQUALIZER_ADAPTATION_DISABLED,
	V32_EQUALIZER_ADAPTATION_COARSE,
	V32_EQUALIZER_ADAPTATION_FINE,
};

/* The decoder must echo this complete observation when it commits a
 * decision.  Lineage is deterministic compatibility evidence, not a secret
 * identity: byte-identical copied owners intentionally remain compatible. */
struct v32_equalizer_observation {
	int16_t i;
	int16_t q;
	uint16_t phase;
	uint16_t magnitude;
	uint64_t epoch;
	uint64_t lineage;
};

struct v32_equalizer_decision {
	struct v32_equalizer_observation observation;
	uint16_t ideal_phase;
	uint16_t ideal_magnitude;
	bool adapt_on_error;
};

/* Persistent three-sample/symbol V.32 fractionally spaced equalizer. */
struct v32_equalizer {
	int16_t i_coefficients[V32_EQUALIZER_TAPS];
	int16_t q_coefficients[V32_EQUALIZER_TAPS];
	int16_t sample_history[V32_EQUALIZER_TAPS];
	struct v32_equalizer_observation pending_observation;
	uint64_t epoch;
	uint64_t lineage;
	int32_t carrier_integrator;
	int32_t carrier_phase_accumulator;
	int16_t smoothed_carrier_error;
	int16_t decision_error;
	int16_t pending_filtered_i;
	int16_t pending_filtered_q;
	int16_t pending_carrier_phase;
	uint16_t carrier_training_symbols;
	uint8_t history_index;
	uint8_t carrier_phase_index;
	uint8_t carrier_gain_index;
	uint8_t samples_until_observation;
	enum v32_equalizer_adaptation adaptation;
	bool carrier_tracking;
	bool decision_pending;
};

enum v32_equalizer_result v32_equalizer_init(
	struct v32_equalizer *equalizer);
bool v32_equalizer_is_valid(
	const struct v32_equalizer *equalizer);

/* Lifecycle changes are accepted only at a decision boundary. */
enum v32_equalizer_result v32_equalizer_set_adaptation(
	struct v32_equalizer *equalizer,
	enum v32_equalizer_adaptation adaptation);
enum v32_equalizer_result v32_equalizer_set_carrier_tracking(
	struct v32_equalizer *equalizer, bool enabled);
enum v32_equalizer_result v32_equalizer_restart_carrier_acquisition(
	struct v32_equalizer *equalizer);

/* Consume no farther than the next decision boundary.  OK reports no
 * observation; OBSERVATION_READY blocks further input until commit.  Every
 * rejected call leaves the owner and both outputs unchanged. */
enum v32_equalizer_result v32_equalizer_process(
	struct v32_equalizer *equalizer,
	const int16_t *samples, size_t count,
	size_t *consumed,
	struct v32_equalizer_observation *observation);

/* Carrier/error state is updated for every accepted decision.  LMS also
 * requires an enabled adaptation mode, the per-decision gate, and the newly
 * smoothed decision-error evidence from that decision. */
enum v32_equalizer_result v32_equalizer_commit(
	struct v32_equalizer *equalizer,
	const struct v32_equalizer_decision *decision);

const char *v32_equalizer_result_name(
	enum v32_equalizer_result result);

#endif
