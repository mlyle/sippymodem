// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V32_CONDITIONING_RECEIVER_H
#define SIPPYMODEM_V32_CONDITIONING_RECEIVER_H

#include "sippymodem_qam_gain.h"
#include "sippymodem_v32_conditioning.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	SIPPYMODEM_V32_CONDITIONING_S_BAR_ACQUIRE_TRANSITIONS = 6,
	SIPPYMODEM_V32_CONDITIONING_PREFIX_MATCH_TRANSITIONS = 32,
};

enum sippymodem_v32_conditioning_receiver_phase {
	SIPPYMODEM_V32_CONDITIONING_SEARCH_S_BAR,
	SIPPYMODEM_V32_CONDITIONING_MATCH_TRN,
	SIPPYMODEM_V32_CONDITIONING_TRAIN_TRN,
	SIPPYMODEM_V32_CONDITIONING_READY,
};

enum sippymodem_v32_conditioning_receiver_result {
	SIPPYMODEM_V32_CONDITIONING_RECEIVER_OK,
	SIPPYMODEM_V32_CONDITIONING_RECEIVER_COMPLETE,
	SIPPYMODEM_V32_CONDITIONING_RECEIVER_INVALID_ARGUMENT,
};

struct sippymodem_v32_conditioning_receiver {
	struct sippymodem_v32_conditioning_transmitter reference;
	struct sippymodem_qam_gain gain;
	struct sippymodem_qam_point previous_point;
	uint8_t prefix_turns[
		SIPPYMODEM_V32_CONDITIONING_PREFIX_MATCH_TRANSITIONS];
	uint8_t prefix_fallback[
		SIPPYMODEM_V32_CONDITIONING_PREFIX_MATCH_TRANSITIONS];
	uint16_t trn_symbols;
	enum sippymodem_v32_transmitter_role source_role;
	enum sippymodem_v32_conditioning_receiver_phase phase;
	uint8_t observed_quadrant;
	uint8_t quadrant_offset;
	uint8_t alternating_transitions;
	uint8_t previous_turn;
	uint8_t prefix_match;
	uint8_t consecutive_mismatches;
	bool has_previous_point;
};

enum sippymodem_v32_conditioning_receiver_result
sippymodem_v32_conditioning_receiver_init(
	struct sippymodem_v32_conditioning_receiver *receiver,
	enum sippymodem_v32_transmitter_role source_role);
bool sippymodem_v32_conditioning_receiver_is_valid(
	const struct sippymodem_v32_conditioning_receiver *receiver);
bool sippymodem_v32_conditioning_receiver_is_complete(
	const struct sippymodem_v32_conditioning_receiver *receiver);

/*
 * Observe one carrier- and timing-recovered point. The receiver first
 * qualifies the alternating S-bar tail, finds a generated TRN prefix without
 * assuming absolute carrier phase, and then tracks the known training stream.
 * Rejected and complete calls leave state and ready unchanged.
 */
enum sippymodem_v32_conditioning_receiver_result
sippymodem_v32_conditioning_receiver_observe(
	struct sippymodem_v32_conditioning_receiver *receiver,
	const struct sippymodem_qam_point *point, bool *ready);

/* Seed rate-signal decoding from the final accepted TRN symbol. */
enum sippymodem_v32_conditioning_receiver_result
sippymodem_v32_conditioning_receiver_framing_state(
	const struct sippymodem_v32_conditioning_receiver *receiver,
	struct sippymodem_v32_symbol_decoder *decoder,
	struct sippymodem_qam_gain *gain);

const char *sippymodem_v32_conditioning_receiver_result_name(
	enum sippymodem_v32_conditioning_receiver_result result);

#endif
