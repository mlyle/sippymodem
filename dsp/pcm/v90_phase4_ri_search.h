// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_PHASE4_RI_SEARCH_H
#define V90_PHASE4_RI_SEARCH_H

#include "v90_phase4_ri.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V90_PHASE4_RI_SEARCH_SYMBOLS = 36,
};

enum v90_phase4_ri_search_stage {
	V90_PHASE4_RI_SEARCH_HUNT,
	V90_PHASE4_RI_SEARCH_AMBIGUOUS,
	V90_PHASE4_RI_SEARCH_BAR,
	V90_PHASE4_RI_SEARCH_COMPLETE,
};

enum v90_phase4_ri_search_result {
	V90_PHASE4_RI_SEARCH_OK,
	V90_PHASE4_RI_SEARCH_ACQUIRED,
	V90_PHASE4_RI_SEARCH_TRANSITION_DETECTED,
	V90_PHASE4_RI_SEARCH_DONE,
	V90_PHASE4_RI_SEARCH_INVALID_ARGUMENT,
	V90_PHASE4_RI_SEARCH_INVALID_STATE,
	V90_PHASE4_RI_SEARCH_INVALID_SEQUENCE,
};

/* Acquire Ri after a preceding, unframed Phase 3 stream. The exact 36-symbol
 * window makes an accidental match in scrambled Phase 3 data negligible.
 * Ri alone cannot distinguish a whole-signal polarity reversal from a
 * three-symbol phase shift, so both equivalent hypotheses are retained until
 * Ri-bar resolves the ambiguity. */
struct v90_phase4_ri_search {
	struct v90_phase4_ri_rx hypotheses[2];
	enum v90_phase4_ri_search_stage stage;
	enum g711_law law;
	uint8_t history[V90_PHASE4_RI_SEARCH_SYMBOLS];
	uint8_t history_count;
	uint8_t history_cursor;
	uint8_t training_ucode;
	uint8_t selected_hypothesis;
};

enum v90_phase4_ri_search_result v90_phase4_ri_search_init(
	struct v90_phase4_ri_search *search, enum g711_law law,
	uint8_t training_ucode);
bool v90_phase4_ri_search_is_valid(
	const struct v90_phase4_ri_search *search);
bool v90_phase4_ri_search_is_acquired(
	const struct v90_phase4_ri_search *search);
bool v90_phase4_ri_search_is_complete(
	const struct v90_phase4_ri_search *search);

/* Consume one raw G.711 character. Wrong magnitudes and non-Ri sign windows
 * remain ordinary search input. Once acquired, a malformed Ri/Ri-bar stream
 * is a protocol error and leaves the search state unchanged. */
enum v90_phase4_ri_search_result v90_phase4_ri_search_receive(
	struct v90_phase4_ri_search *search, uint8_t network_octet);

const char *v90_phase4_ri_search_result_name(
	enum v90_phase4_ri_search_result result);

#endif /* V90_PHASE4_RI_SEARCH_H */
