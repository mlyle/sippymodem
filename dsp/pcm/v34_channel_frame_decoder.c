// SPDX-License-Identifier: BSD-3-Clause

#include "v34_channel_frame_decoder.h"

#include "memory_region.h"

#include <stddef.h>
#include <string.h>

static bool mapping_frames_are_equal(
	const struct v34_mapping_frame *left,
	const struct v34_mapping_frame *right)
{
	return left->index == right->index &&
	       left->data_bits == right->data_bits &&
	       left->primary_bits == right->primary_bits &&
	       left->inversion_4d_mask == right->inversion_4d_mask &&
	       left->high_frame == right->high_frame &&
	       left->carries_auxiliary_bit == right->carries_auxiliary_bit &&
	       left->completes_data_frame == right->completes_data_frame;
}

static bool channel_frame_matches_schedule(
	const struct v34_channel_frame_decoder *decoder)
{
	struct v34_frame_schedule before;
	struct v34_mapping_frame expected;

	if (!v34_frame_schedule_is_valid(
		    &decoder->channel_schedule) ||
	    decoder->channel_schedule.next_mapping_frame == 0)
		return false;
	before = decoder->channel_schedule;
	before.next_mapping_frame--;
	if (v34_frame_schedule_next(&before, &expected) !=
		    V34_FRAME_SCHEDULE_OK ||
	    before.data_frame_in_superframe !=
		    decoder->channel_schedule.data_frame_in_superframe ||
	    before.next_mapping_frame !=
		    decoder->channel_schedule.next_mapping_frame)
		return false;
	return mapping_frames_are_equal(&decoder->channel_frame, &expected);
}

static bool decided_interval_is_bounded(
	const struct v34_decided_4d_interval *decided)
{
	if (decided->trellis_output > 1u)
		return false;
	for (uint8_t symbol = 0;
	     symbol < V34_2D_SYMBOLS_PER_4D_INTERVAL; ++symbol) {
		const struct v34_grid_point *point =
			&decided->mapper_output[symbol];

		if (!v34_grid_point_coordinates_are_odd(point))
			return false;
	}
	return true;
}

static bool cursors_are_aligned(
	const struct v34_channel_frame_decoder *decoder)
{
	const struct v34_channel_decoder *channel =
		&decoder->channel_decoder;
	const struct v34_survivor *survivor = &channel->survivor;

	if (decoder->next_channel_interval !=
	    survivor->path_index %
		    V34_4D_INTERVALS_PER_MAPPING_FRAME)
		return false;

	if (survivor->path_intervals < V34_SURVIVOR_TRACEBACK_INTERVALS)
		return decoder->pending_interval_count == 0u;
	return decoder->pending_interval_count ==
	       (uint8_t)((decoder->next_channel_interval + 1u) %
			 V34_4D_INTERVALS_PER_MAPPING_FRAME);
}

bool v34_channel_frame_decoder_is_valid(
	const struct v34_channel_frame_decoder *decoder)
{
	if (decoder == NULL ||
	    !v34_channel_decoder_is_valid(
		    &decoder->channel_decoder) ||
	    decoder->next_channel_interval >=
		    V34_4D_INTERVALS_PER_MAPPING_FRAME ||
	    decoder->pending_interval_count >=
		    V34_4D_INTERVALS_PER_MAPPING_FRAME ||
	    !channel_frame_matches_schedule(decoder) ||
	    !v34_frame_geometry_is_equal(
		    &decoder->channel_decoder.geometry,
		    &decoder->channel_schedule.geometry) ||
	    !cursors_are_aligned(decoder))
		return false;
	for (uint8_t interval = 0;
	     interval < decoder->pending_interval_count; ++interval) {
		if (!decided_interval_is_bounded(
			    &decoder->pending_frame[interval]))
			return false;
	}
	return true;
}

static bool load_next_channel_frame(
	struct v34_frame_schedule *schedule,
	struct v34_mapping_frame *frame)
{
	if (v34_frame_schedule_prepare_mapping_frame(schedule) !=
	    V34_FRAME_SCHEDULE_OK)
		return false;
	return v34_frame_schedule_next(schedule, frame) ==
	       V34_FRAME_SCHEDULE_OK;
}

enum v34_channel_frame_decoder_result
v34_channel_frame_decoder_init(
	struct v34_channel_frame_decoder *decoder,
	const struct v34_frame_geometry *geometry,
	bool expanded_shaping,
	const struct v34_q14_coefficient
		coefficients[V34_PRECODER_TAP_COUNT],
	enum v34_trellis_selection selection)
{
	struct v34_channel_frame_decoder initialized = { 0 };
	if (decoder == NULL ||
	    v34_channel_decoder_init(
		    &initialized.channel_decoder, geometry, expanded_shaping,
		    coefficients, selection) != V34_CHANNEL_DECODER_OK ||
	    v34_b1_frame_schedule_init(
		    &initialized.channel_schedule, geometry) !=
		    V34_FRAME_SCHEDULE_OK ||
	    !load_next_channel_frame(
		    &initialized.channel_schedule, &initialized.channel_frame) ||
	    !v34_channel_frame_decoder_is_valid(&initialized))
		return V34_CHANNEL_FRAME_DECODER_INVALID_ARGUMENT;
	*decoder = initialized;
	return V34_CHANNEL_FRAME_DECODER_OK;
}

static enum v34_channel_frame_decoder_result channel_result(
	enum v34_channel_decoder_result result)
{
	switch (result) {
	case V34_CHANNEL_DECODER_NUMERIC_FAILURE:
		return V34_CHANNEL_FRAME_DECODER_NUMERIC_FAILURE;
	case V34_CHANNEL_DECODER_OUT_OF_RANGE:
		return V34_CHANNEL_FRAME_DECODER_OUT_OF_RANGE;
	case V34_CHANNEL_DECODER_OK:
	case V34_CHANNEL_DECODER_INVALID_ARGUMENT:
		break;
	}
	return V34_CHANNEL_FRAME_DECODER_INVALID_ARGUMENT;
}

static enum v34_channel_frame_decoder_result advance(
	struct v34_channel_frame_decoder *decoder,
	const struct qam_point
		estimates[V34_2D_SYMBOLS_PER_4D_INTERVAL],
	const struct qam_point
		normalized[V34_2D_SYMBOLS_PER_4D_INTERVAL],
	const struct v34_power_normalizer *normalizer,
	struct v34_survivor_decision *current,
	struct v34_decided_4d_interval
		decided_frame[V34_4D_INTERVALS_PER_MAPPING_FRAME],
	bool *frame_ready)
{
	const size_t estimates_size =
		sizeof(*estimates) * V34_2D_SYMBOLS_PER_4D_INTERVAL;
	const struct memory_region regions[] = {
		{ decoder, sizeof(*decoder) },
		{ estimates, estimates_size },
		{ normalized, normalized == NULL ? 0u : estimates_size },
		{ normalizer, normalizer == NULL ? 0u : sizeof(*normalizer) },
		{ current, sizeof(*current) },
		{ decided_frame,
		  sizeof(*decided_frame) * V34_4D_INTERVALS_PER_MAPPING_FRAME },
		{ frame_ready, sizeof(*frame_ready) },
	};
	struct v34_frame_schedule next_schedule;
	struct v34_mapping_frame next_frame;
	struct v34_survivor_decision generated_current;
	struct v34_decided_4d_interval generated_decided;
	enum v34_channel_decoder_result result;
	uint8_t inversion_bit;
	uint8_t next_interval;
	bool decided_ready;
	bool generated_frame_ready = false;

	if (!v34_channel_frame_decoder_is_valid(decoder) ||
	    estimates == NULL || current == NULL || decided_frame == NULL ||
	    frame_ready == NULL ||
	    memory_region_set_overlaps(regions, sizeof regions / sizeof *regions))
		return V34_CHANNEL_FRAME_DECODER_INVALID_ARGUMENT;
	inversion_bit = (uint8_t)(
		(decoder->channel_frame.inversion_4d_mask >>
		 decoder->next_channel_interval) &
		1u);
	/* Prepare the only fallible framing work before the child transaction.
	 * Successful decoding leaves only bounded cursor and pending-frame updates;
	 * the traceback history does not need a second snapshot here. */
	next_schedule = decoder->channel_schedule;
	next_frame = decoder->channel_frame;
	next_interval = (uint8_t)((decoder->next_channel_interval + 1u) %
				 V34_4D_INTERVALS_PER_MAPPING_FRAME);
	if (next_interval == 0u &&
	    !load_next_channel_frame(&next_schedule, &next_frame))
		return V34_CHANNEL_FRAME_DECODER_INVALID_ARGUMENT;
	result = normalized == NULL ?
		v34_channel_decoder_advance(
			&decoder->channel_decoder, estimates, inversion_bit,
			&generated_current, &generated_decided, &decided_ready) :
		v34_channel_decoder_advance_projected(
			&decoder->channel_decoder, estimates, normalized, normalizer,
			inversion_bit, &generated_current, &generated_decided,
			&decided_ready);
	if (result != V34_CHANNEL_DECODER_OK)
		return channel_result(result);
	decoder->channel_schedule = next_schedule;
	decoder->channel_frame = next_frame;
	decoder->next_channel_interval = next_interval;
	if (decided_ready) {
		decoder->pending_frame[decoder->pending_interval_count++] =
			generated_decided;
		if (decoder->pending_interval_count ==
		    V34_4D_INTERVALS_PER_MAPPING_FRAME) {
			decoder->pending_interval_count = 0;
			generated_frame_ready = true;
		}
	}
	*current = generated_current;
	*frame_ready = generated_frame_ready;
	if (generated_frame_ready)
		memcpy(decided_frame, decoder->pending_frame,
		       sizeof(decoder->pending_frame));
	return V34_CHANNEL_FRAME_DECODER_OK;
}

enum v34_channel_frame_decoder_result
v34_channel_frame_decoder_advance(
	struct v34_channel_frame_decoder *decoder,
	const struct qam_point estimates[V34_2D_SYMBOLS_PER_4D_INTERVAL],
	struct v34_survivor_decision *current,
	struct v34_decided_4d_interval
		decided_frame[V34_4D_INTERVALS_PER_MAPPING_FRAME],
	bool *frame_ready)
{
	return advance(
		decoder, estimates, NULL, NULL, current, decided_frame,
		frame_ready);
}

enum v34_channel_frame_decoder_result
v34_channel_frame_decoder_advance_projected(
	struct v34_channel_frame_decoder *decoder,
	const struct qam_point estimates[V34_2D_SYMBOLS_PER_4D_INTERVAL],
	const struct qam_point normalized[V34_2D_SYMBOLS_PER_4D_INTERVAL],
	const struct v34_power_normalizer *normalizer,
	struct v34_survivor_decision *current,
	struct v34_decided_4d_interval
		decided_frame[V34_4D_INTERVALS_PER_MAPPING_FRAME],
	bool *frame_ready)
{
	if (normalized == NULL || !v34_power_normalizer_is_valid(normalizer))
		return V34_CHANNEL_FRAME_DECODER_INVALID_ARGUMENT;
	return advance(
		decoder, estimates, normalized, normalizer, current, decided_frame,
		frame_ready);
}
