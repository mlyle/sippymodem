// SPDX-License-Identifier: BSD-3-Clause

#ifndef V32_CONDITIONING_RECEIVER_H
#define V32_CONDITIONING_RECEIVER_H

#include "qam_gain.h"
#include "conditioning.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V32_CONDITIONING_S_BAR_ACQUIRE_TRANSITIONS = 6,
	V32_CONDITIONING_PREFIX_MATCH_TRANSITIONS = 32,
};

enum v32_conditioning_receiver_phase {
	V32_CONDITIONING_SEARCH_S_BAR,
	V32_CONDITIONING_MATCH_TRN,
	V32_CONDITIONING_TRAIN_TRN,
	V32_CONDITIONING_READY,
};

enum v32_conditioning_receiver_result {
	V32_CONDITIONING_RECEIVER_OK,
	V32_CONDITIONING_RECEIVER_COMPLETE,
	V32_CONDITIONING_RECEIVER_INVALID_ARGUMENT,
};

struct v32_conditioning_receiver {
	struct v32_conditioning_transmitter reference;
	struct qam_gain gain;
	struct qam_point previous_point;
	uint8_t prefix_turns[
		V32_CONDITIONING_PREFIX_MATCH_TRANSITIONS];
	uint8_t prefix_fallback[
		V32_CONDITIONING_PREFIX_MATCH_TRANSITIONS];
	uint16_t trn_symbols;
	enum v32_transmitter_role source_role;
	enum v32_conditioning_receiver_phase phase;
	uint8_t observed_quadrant;
	uint8_t quadrant_offset;
	uint8_t pending_quadrant_offset;
	uint8_t alternating_transitions;
	uint8_t previous_turn;
	uint8_t prefix_match;
	uint8_t consecutive_mismatches;
	uint8_t phase_corrections;
	bool has_previous_point;
};

enum v32_conditioning_receiver_result
v32_conditioning_receiver_init(
	struct v32_conditioning_receiver *receiver,
	enum v32_transmitter_role source_role);
bool v32_conditioning_receiver_is_valid(
	const struct v32_conditioning_receiver *receiver);
bool v32_conditioning_receiver_is_complete(
	const struct v32_conditioning_receiver *receiver);

/*
 * Observe one carrier- and timing-recovered point. The receiver first
 * qualifies the alternating S-bar tail, finds a generated TRN prefix without
 * assuming absolute carrier phase, and then tracks the known training stream.
 * Rejected and complete calls leave state and ready unchanged.
 */
enum v32_conditioning_receiver_result
v32_conditioning_receiver_observe(
	struct v32_conditioning_receiver *receiver,
	const struct qam_point *point, bool *ready);

/* Seed rate-signal decoding from the final accepted TRN symbol. */
enum v32_conditioning_receiver_result
v32_conditioning_receiver_framing_state(
	const struct v32_conditioning_receiver *receiver,
	struct v32_symbol_decoder *decoder,
	struct qam_gain *gain);

const char *v32_conditioning_receiver_result_name(
	enum v32_conditioning_receiver_result result);

#endif
