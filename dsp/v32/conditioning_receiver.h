// SPDX-License-Identifier: BSD-3-Clause

#ifndef V32_CONDITIONING_RECEIVER_H
#define V32_CONDITIONING_RECEIVER_H

#include "conditioning.h"
#include "equalizer.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V32_CONDITIONING_S_DETECT_TRANSITIONS = 6,
	V32_CONDITIONING_S_EPOCH_TRANSITIONS =
		V32_CONDITIONING_S_SYMBOLS / 2,
	V32_CONDITIONING_PREFIX_MATCH_TRANSITIONS = 32,
};

enum v32_conditioning_receiver_phase {
	V32_CONDITIONING_SEARCH_S_BAR,
	V32_CONDITIONING_MATCH_TRN,
	V32_CONDITIONING_COUNT_S_BAR,
	V32_CONDITIONING_TRAIN_TRN,
	V32_CONDITIONING_READY,
};

enum v32_conditioning_receiver_result {
	V32_CONDITIONING_RECEIVER_OK,
	V32_CONDITIONING_RECEIVER_COMPLETE,
	V32_CONDITIONING_RECEIVER_INVALID_ARGUMENT,
};

struct v32_conditioning_epoch_detector {
	uint16_t phase;
	uint16_t timer;
	int16_t i_last;
	int16_t q_last;
	int16_t i_previous;
	int16_t q_previous;
	int16_t i_older;
	int16_t q_older;
	int16_t average_a;
	int16_t average_b;
};

struct v32_conditioning_receiver {
	struct v32_conditioning_transmitter reference;
	struct v32_conditioning_epoch_detector epoch_detector;
	uint8_t prefix_turns[
		V32_CONDITIONING_PREFIX_MATCH_TRANSITIONS];
	uint8_t prefix_fallback[
		V32_CONDITIONING_PREFIX_MATCH_TRANSITIONS];
	uint64_t observation_epoch;
	uint64_t observation_lineage;
	uint16_t trn_symbols;
	enum v32_transmitter_role source_role;
	enum v32_conditioning_receiver_phase phase;
	uint8_t previous_quadrant;
	uint8_t quadrant_offset;
	uint8_t pending_quadrant_offset;
	uint8_t alternating_transitions;
	uint8_t s_bar_symbols;
	uint8_t previous_turn;
	uint8_t prefix_match;
	uint8_t consecutive_mismatches;
	uint16_t confirmed_trn_symbols;
	uint16_t stable_trn_symbols;
	bool has_previous_quadrant;
};

/* One accepted line observation always yields exactly one commit-ready
 * equalizer decision.  The nominal reference metadata is also used by the
 * temporary QAM bridge to retain its flat-channel estimate without reaching
 * into receiver state. */
struct v32_conditioning_symbol_decision {
	struct v32_equalizer_decision equalizer;
	uint8_t reference_quadrant;
	bool reference_known;
	bool reference_confirmed;
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
 * Observe one persistent-equalizer decision point. The receiver first
 * qualifies S, uses the standard S-to-S-bar epoch when it is observed, and
 * retains exact generated-prefix acquisition for inputs which begin in TRN.
 * It then tracks the known training stream, returning the nominal decision so
 * the persistent equalizer removes any observed quarter-turn carrier offset.
 * Ready means local training is sufficient to begin seeking a rate signal;
 * the remote transmitter may continue TRN. OK consumes exactly one consecutive
 * observation and writes one decision. Observation lineage is retained and
 * echoed, but may evolve after each equalizer commit; the equalizer validates
 * that ownership when the decision is committed. Rejected and complete calls
 * leave the receiver and output unchanged.
 */
enum v32_conditioning_receiver_result
v32_conditioning_receiver_observe(
	struct v32_conditioning_receiver *receiver,
	const struct v32_equalizer_observation *observation,
	struct v32_conditioning_symbol_decision *decision);

/* Seed the independent rate-signal differential phase at its defined zero. */
enum v32_conditioning_receiver_result
v32_conditioning_receiver_framing_state(
	const struct v32_conditioning_receiver *receiver,
	struct v32_symbol_decoder *decoder);

#endif
