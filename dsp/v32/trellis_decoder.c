// SPDX-License-Identifier: BSD-3-Clause

#include "trellis_decoder.h"

#include "constellation.h"
#include "integer_math.h"
#include "memory_region.h"
#include "trellis_profile.h"

#include <limits.h>
#include <stddef.h>

enum {
	METRIC_DECAY_Q15 = 0x799a,
	BRANCH_SCALE = 0x0666,
};

static const uint8_t BRANCH_FOR_STATE[V32_TRELLIS_STATE_COUNT][4] = {
	{ 0, 1, 2, 3 },
	{ 0, 1, 2, 3 },
	{ 1, 0, 3, 2 },
	{ 1, 0, 3, 2 },
	{ 2, 3, 0, 1 },
	{ 3, 2, 1, 0 },
	{ 3, 2, 1, 0 },
	{ 2, 3, 0, 1 },
};

/* Rows are the prior received quadrant; columns are the coded quadrant. */
static const uint8_t DIFFERENTIAL_DECODE[4][4] = {
	{ 0, 1, 2, 3 },
	{ 1, 0, 3, 2 },
	{ 3, 2, 0, 1 },
	{ 2, 3, 1, 0 },
};

static bool decoder_header_is_valid(
	const struct v32_trellis_decoder *decoder)
{
	return decoder != NULL &&
	       v32_trellis_profile_for_data_bits(
		       decoder->data_bits_per_symbol) != NULL &&
	       decoder->active_slot < V32_TRELLIS_DECODER_TRACEBACK_SYMBOLS &&
	       decoder->history_fill <=
		       V32_TRELLIS_DECODER_TRACEBACK_SYMBOLS &&
	       (decoder->history_fill ==
			V32_TRELLIS_DECODER_TRACEBACK_SYMBOLS ||
		decoder->active_slot == decoder->history_fill) &&
	       decoder->previous_differential_output <
		       V32_TRELLIS_QUADRANT_COUNT;
}

bool v32_trellis_decoder_is_valid(
	const struct v32_trellis_decoder *decoder)
{
	uint8_t signal_count;

	if (!decoder_header_is_valid(decoder))
		return false;
	signal_count = (uint8_t)(UINT8_C(1) <<
				 (decoder->data_bits_per_symbol + 1u));
	for (size_t slot = 0;
	     slot < V32_TRELLIS_DECODER_TRACEBACK_SYMBOLS; ++slot) {
		for (size_t state = 0;
		     state < V32_TRELLIS_STATE_COUNT; ++state) {
			const struct v32_trellis_decoder_path *path =
				&decoder->paths[slot][state];

			if (path->signal_symbol >= signal_count ||
			    path->predecessor >= V32_TRELLIS_STATE_COUNT)
				return false;
		}
	}
	return true;
}

enum v32_trellis_decoder_result v32_trellis_decoder_init(
	struct v32_trellis_decoder *decoder,
	uint8_t data_bits_per_symbol,
	uint8_t previous_differential_output)
{
	struct v32_trellis_decoder initialized = {
		.previous_differential_output = previous_differential_output,
		.data_bits_per_symbol = data_bits_per_symbol,
	};

	if (decoder == NULL ||
	    v32_trellis_profile_for_data_bits(data_bits_per_symbol) == NULL ||
	    previous_differential_output >= V32_TRELLIS_QUADRANT_COUNT)
		return V32_TRELLIS_DECODER_INVALID_ARGUMENT;
	if (!v32_trellis_decoder_is_valid(&initialized))
		return V32_TRELLIS_DECODER_INVALID_STATE;
	*decoder = initialized;
	return V32_TRELLIS_DECODER_OK;
}

static bool observation_is_valid(
	const struct v32_equalizer_observation *observation)
{
	return observation != NULL && observation->epoch != 0 &&
	       observation->phase <= INT16_MAX &&
	       observation->magnitude <= INT16_MAX;
}

static bool decode_arguments_are_separate(
	const struct v32_trellis_decoder *decoder,
	const struct v32_equalizer_observation *observation,
	const struct v32_trellis_decoded_symbol *symbol)
{
	return observation != NULL && symbol != NULL &&
	       !memory_regions_overlap(
		       decoder, sizeof(*decoder),
		       observation, sizeof(*observation)) &&
	       !memory_regions_overlap(
		       decoder, sizeof(*decoder), symbol, sizeof(*symbol)) &&
	       !memory_regions_overlap(
		       observation, sizeof(*observation),
		       symbol, sizeof(*symbol));
}

static int16_t branch_metric(
	const struct v32_equalizer_observation *observation,
	struct v32_fixed_point point)
{
	int64_t difference_i = (int32_t)observation->i - point.i;
	int64_t difference_q = (int32_t)observation->q - point.q;
	uint64_t energy = (uint64_t)(difference_i * difference_i +
				     difference_q * difference_q);
	uint64_t scaled = ((energy >> 16) * BRANCH_SCALE) >> 12;

	return wrap_s16_from_s32((int32_t)scaled);
}

static void select_path(
	struct v32_trellis_decoder *decoder, uint8_t slot, uint8_t state,
	const int16_t previous[V32_TRELLIS_STATE_COUNT],
	const uint8_t candidates[V32_TRELLIS_PROFILE_CANDIDATES],
	const int16_t branch[V32_TRELLIS_PROFILE_CANDIDATES])
{
	uint8_t predecessor_base = (state & 1u) != 0 ? 4 : 0;
	uint8_t selected_predecessor = predecessor_base;
	uint8_t selected_candidate = (uint8_t)(
		predecessor_base + BRANCH_FOR_STATE[state][0]);
	int16_t selected_metric = wrap_s16_from_s32(
		(int32_t)previous[selected_predecessor] +
		branch[selected_candidate]);

	for (uint8_t offset = 1; offset < 4; ++offset) {
		uint8_t predecessor = (uint8_t)(predecessor_base + offset);
		uint8_t candidate = (uint8_t)(
			predecessor_base + BRANCH_FOR_STATE[state][offset]);
		int16_t metric = wrap_s16_from_s32(
			(int32_t)previous[predecessor] + branch[candidate]);

		if (metric < selected_metric) {
			selected_metric = metric;
			selected_predecessor = predecessor;
			selected_candidate = candidate;
		}
	}
	decoder->path_metrics[state] = selected_metric;
	decoder->paths[slot][state] =
		(struct v32_trellis_decoder_path){
			.signal_symbol = candidates[selected_candidate],
			.predecessor = selected_predecessor,
		};
}

static enum v32_trellis_decoder_result advance_paths(
	struct v32_trellis_decoder *decoder,
	const struct v32_equalizer_observation *observation,
	uint8_t old_symbols[V32_TRELLIS_STATE_COUNT], uint8_t *best_state)
{
	const struct v32_trellis_profile *profile =
		v32_trellis_profile_for_data_bits(
			decoder->data_bits_per_symbol);
	uint8_t candidates[V32_TRELLIS_PROFILE_CANDIDATES];
	int16_t branch[V32_TRELLIS_PROFILE_CANDIDATES];
	int16_t previous[V32_TRELLIS_STATE_COUNT];
	uint8_t slot = (uint8_t)((decoder->active_slot + 1u) &
				 (V32_TRELLIS_DECODER_TRACEBACK_SYMBOLS - 1u));

	if (v32_trellis_profile_candidates(
		    profile, observation->i, observation->q, candidates) !=
	    V32_TRELLIS_PROFILE_OK)
		return V32_TRELLIS_DECODER_INVALID_STATE;
	for (uint8_t state = 0; state < V32_TRELLIS_STATE_COUNT; ++state) {
		struct v32_fixed_point point;

		previous[state] = wrap_s16_from_s32(
			arithmetic_shift_right_u32(
				(uint32_t)((int32_t)decoder->path_metrics[state] *
					   METRIC_DECAY_Q15),
				15));
		old_symbols[state] = decoder->paths[slot][state].signal_symbol;
		if (v32_trellis_fixed_signal_point(
			    decoder->data_bits_per_symbol, candidates[state],
			    &point) != V32_CONSTELLATION_OK)
			return V32_TRELLIS_DECODER_INVALID_STATE;
		branch[state] = branch_metric(observation, point);
	}
	decoder->active_slot = slot;
	for (uint8_t state = 0; state < V32_TRELLIS_STATE_COUNT; ++state)
		select_path(
			decoder, slot, state, previous, candidates, branch);
	*best_state = 0;
	for (uint8_t state = 1; state < V32_TRELLIS_STATE_COUNT; ++state) {
		if (decoder->path_metrics[state] <
		    decoder->path_metrics[*best_state])
			*best_state = state;
	}
	return V32_TRELLIS_DECODER_OK;
}

static enum v32_trellis_decoder_result trace_symbol(
	const struct v32_trellis_decoder *decoder, uint8_t best_state,
	const uint8_t old_symbols[V32_TRELLIS_STATE_COUNT],
	uint8_t *signal_symbol)
{
	uint8_t slot = decoder->active_slot;
	uint8_t signal_count = (uint8_t)(UINT8_C(1) <<
				 (decoder->data_bits_per_symbol + 1u));

	for (uint8_t step = 0;
	     step < V32_TRELLIS_DECODER_TRACEBACK_SYMBOLS; ++step) {
		const struct v32_trellis_decoder_path *path =
			&decoder->paths[slot][best_state];

		if (path->signal_symbol >= signal_count ||
		    path->predecessor >= V32_TRELLIS_STATE_COUNT)
			return V32_TRELLIS_DECODER_INVALID_STATE;
		best_state = path->predecessor;
		slot = (uint8_t)((slot - 1u) &
				 (V32_TRELLIS_DECODER_TRACEBACK_SYMBOLS - 1u));
	}
	if (old_symbols[best_state] >= signal_count)
		return V32_TRELLIS_DECODER_INVALID_STATE;
	*signal_symbol = old_symbols[best_state];
	return V32_TRELLIS_DECODER_OK;
}

static uint8_t decode_group(
	struct v32_trellis_decoder *decoder, uint8_t signal_symbol)
{
	uint8_t shift = (uint8_t)(decoder->data_bits_per_symbol - 2u);
	uint8_t data_mask = (uint8_t)(
		(UINT8_C(1) << decoder->data_bits_per_symbol) - 1u);
	uint8_t low_mask = (uint8_t)((UINT8_C(1) << shift) - 1u);
	uint8_t masked = (uint8_t)(signal_symbol & data_mask);
	uint8_t coded_quadrant = (uint8_t)(masked >> shift);
	uint8_t decoded_quadrant =
		DIFFERENTIAL_DECODE[decoder->previous_differential_output]
				   [coded_quadrant];

	decoder->previous_differential_output = coded_quadrant;
	return (uint8_t)((decoded_quadrant << shift) |
			 (masked & low_mask));
}

enum v32_trellis_decoder_result v32_trellis_decoder_decode(
	struct v32_trellis_decoder *decoder,
	const struct v32_equalizer_observation *observation,
	struct v32_trellis_decoded_symbol *symbol)
{
	struct v32_trellis_decoder next;
	struct v32_trellis_decoded_symbol next_symbol = { 0 };
	uint8_t old_symbols[V32_TRELLIS_STATE_COUNT];
	uint8_t best_state;
	uint8_t signal_symbol;

	if (!v32_trellis_decoder_is_valid(decoder))
		return V32_TRELLIS_DECODER_INVALID_STATE;
	if (!decode_arguments_are_separate(decoder, observation, symbol) ||
	    !observation_is_valid(observation))
		return V32_TRELLIS_DECODER_INVALID_ARGUMENT;
	next = *decoder;
	if (advance_paths(
		    &next, observation, old_symbols, &best_state) !=
	    V32_TRELLIS_DECODER_OK)
		return V32_TRELLIS_DECODER_INVALID_STATE;
	if (next.history_fill < V32_TRELLIS_DECODER_TRACEBACK_SYMBOLS) {
		++next.history_fill;
	} else {
		if (trace_symbol(
			    &next, best_state, old_symbols, &signal_symbol) !=
		    V32_TRELLIS_DECODER_OK)
			return V32_TRELLIS_DECODER_INVALID_STATE;
		next_symbol.scrambled_group = decode_group(&next, signal_symbol);
		next_symbol.ready = true;
	}
	if (!decoder_header_is_valid(&next))
		return V32_TRELLIS_DECODER_INVALID_STATE;
	*decoder = next;
	*symbol = next_symbol;
	return V32_TRELLIS_DECODER_OK;
}
