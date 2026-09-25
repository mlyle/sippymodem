// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_SPECTRAL_SHAPER_H
#define V90_SPECTRAL_SHAPER_H

#include "v90_pcm_mapping.h"
#include "v90_pcm_symbol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	V90_SPECTRAL_MAX_FRAME_SYMBOLS = V90_CP_FRAME_INTERVALS,
	V90_SPECTRAL_MAX_LOOKAHEAD = 3,
	V90_SPECTRAL_MAX_BUFFERED_FRAMES =
		V90_SPECTRAL_MAX_LOOKAHEAD + 1,
};

enum v90_spectral_rule {
	V90_SPECTRAL_RULE_A,
	V90_SPECTRAL_RULE_B,
	V90_SPECTRAL_RULE_C,
	V90_SPECTRAL_RULE_D,
};

/* Applying the same sign rule twice recovers the initial sign. */
static inline uint8_t v90_spectral_rule_apply(
	uint8_t sign, size_t position, enum v90_spectral_rule rule)
{
	bool invert = rule == V90_SPECTRAL_RULE_B ||
		      (rule == V90_SPECTRAL_RULE_C &&
		       (position & 1u) == 0) ||
		      (rule == V90_SPECTRAL_RULE_D &&
		       (position & 1u) != 0);

	return sign ^ (uint8_t)invert;
}

enum v90_spectral_shaper_result {
	V90_SPECTRAL_SHAPER_OK,
	V90_SPECTRAL_SHAPER_FRAME_READY,
	V90_SPECTRAL_SHAPER_BUFFERING,
	V90_SPECTRAL_SHAPER_COMPLETE,
	V90_SPECTRAL_SHAPER_INVALID_ARGUMENT,
	V90_SPECTRAL_SHAPER_INVALID_PARAMETERS,
	V90_SPECTRAL_SHAPER_INVALID_STATE,
	V90_SPECTRAL_SHAPER_OUT_OF_RANGE,
	V90_SPECTRAL_SHAPER_NUMERIC_FAILURE,
};

struct v90_spectral_frame {
	uint8_t ucodes[V90_SPECTRAL_MAX_FRAME_SYMBOLS];
	uint8_t initial_signs[V90_SPECTRAL_MAX_FRAME_SYMBOLS];
};

/* One selected shaping frame. Symbols beyond symbol_count are zeroed. The
 * rule is retained as a compact recording and diagnostics seam. */
struct v90_spectral_output {
	struct v90_pcm_symbol symbols[V90_SPECTRAL_MAX_FRAME_SYMBOLS];
	enum v90_spectral_rule rule;
	uint8_t symbol_count;
};

/* Owned spectral-filter and bounded look-ahead state. The largest negotiated
 * window is four shaping frames, so no dynamic storage is required. */
struct v90_spectral_shaper {
	struct v90_spectral_frame
		pending[V90_SPECTRAL_MAX_BUFFERED_FRAMES];
	double previous_x;
	double previous_y;
	double previous_v;
	enum g711_law law;
	int8_t shaping_a1_q1_6;
	int8_t shaping_a2_q1_6;
	int8_t shaping_b1_q1_6;
	int8_t shaping_b2_q1_6;
	uint8_t sign_redundancy;
	uint8_t lookahead;
	uint8_t pending_count;
	bool trellis_state;
	bool finishing;
};

enum v90_spectral_shaper_result v90_spectral_shaper_init(
	struct v90_spectral_shaper *shaper,
	const struct v90_pcm_mapping *mapping);
bool v90_spectral_shaper_is_valid(
	const struct v90_spectral_shaper *shaper);

/* Accept one shaping frame containing 6/Sr unsigned mapper outputs and
 * initial t signs. BUFFERING means the frame was accepted but the negotiated
 * look-ahead is not full. Rejected calls leave both state and output intact. */
enum v90_spectral_shaper_result v90_spectral_shaper_push(
	struct v90_spectral_shaper *shaper,
	const uint8_t *ucodes, const uint8_t *initial_signs,
	size_t symbol_count, struct v90_spectral_output *output);

/* Drain accepted tail frames with the longest look-ahead still available.
 * Once finish is first called, no additional frame may be pushed. */
enum v90_spectral_shaper_result v90_spectral_shaper_finish(
	struct v90_spectral_shaper *shaper,
	struct v90_spectral_output *output);

const char *v90_spectral_shaper_result_name(
	enum v90_spectral_shaper_result result);

#endif /* V90_SPECTRAL_SHAPER_H */
