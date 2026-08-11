// SPDX-License-Identifier: BSD-3-Clause

#include "v34_channel_frame_decoder.h"

#include "sippymodem_v34_mapping_parameters.h"

#include <stddef.h>
#include <string.h>

static bool mapping_parameters_are_equal(
	const struct sippymodem_v34_mapping_parameters *left,
	const struct sippymodem_v34_mapping_parameters *right)
{
	return left->constellation_points == right->constellation_points &&
	       left->mapping_frame_bits == right->mapping_frame_bits &&
	       left->shell_mapper_bits == right->shell_mapper_bits &&
	       left->point_index_bits_per_2d_symbol ==
		       right->point_index_bits_per_2d_symbol &&
	       left->ring_count == right->ring_count &&
	       left->expanded_shaping == right->expanded_shaping;
}

static bool mapping_frames_are_equal(
	const struct sippymodem_v34_mapping_frame *left,
	const struct sippymodem_v34_mapping_frame *right)
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
	const struct sippymodem_v34_channel_frame_decoder *decoder)
{
	struct sippymodem_v34_frame_schedule before;
	struct sippymodem_v34_mapping_frame expected;

	if (!sippymodem_v34_frame_schedule_is_valid(
		    &decoder->channel_schedule) ||
	    decoder->channel_schedule.next_mapping_frame == 0)
		return false;
	before = decoder->channel_schedule;
	before.next_mapping_frame--;
	if (sippymodem_v34_frame_schedule_next(&before, &expected) !=
		    SIPPYMODEM_V34_FRAME_SCHEDULE_OK ||
	    before.data_frame_in_superframe !=
		    decoder->channel_schedule.data_frame_in_superframe ||
	    before.next_mapping_frame !=
		    decoder->channel_schedule.next_mapping_frame)
		return false;
	return mapping_frames_are_equal(&decoder->channel_frame, &expected);
}

static bool decided_interval_is_bounded(
	const struct sippymodem_v34_decided_4d_interval *decided)
{
	if (decided->trellis_output > 1u)
		return false;
	for (uint8_t symbol = 0;
	     symbol < SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL; ++symbol) {
		const struct sippymodem_v34_grid_point *point =
			&decided->mapper_output[symbol];

		if (((uint16_t)point->in_phase & 1u) == 0u ||
		    ((uint16_t)point->quadrature & 1u) == 0u)
			return false;
	}
	return true;
}

static bool cursors_are_aligned(
	const struct sippymodem_v34_channel_frame_decoder *decoder)
{
	const struct sippymodem_v34_survivor *survivor =
		&decoder->channel_decoder.survivor;
	uint8_t history = survivor->path_intervals;

	if (decoder->next_channel_interval !=
	    survivor->path_index %
		    SIPPYMODEM_V34_4D_INTERVALS_PER_MAPPING_FRAME)
		return false;

	if (history < SIPPYMODEM_V34_SURVIVOR_TRACEBACK_INTERVALS)
		return decoder->pending_interval_count == 0u;
	return decoder->pending_interval_count ==
	       (uint8_t)((decoder->next_channel_interval + 1u) %
			 SIPPYMODEM_V34_4D_INTERVALS_PER_MAPPING_FRAME);
}

bool sippymodem_v34_channel_frame_decoder_is_valid(
	const struct sippymodem_v34_channel_frame_decoder *decoder)
{
	struct sippymodem_v34_mapping_parameters expected;

	if (decoder == NULL ||
	    !sippymodem_v34_channel_decoder_is_valid(
		    &decoder->channel_decoder) ||
	    decoder->next_channel_interval >=
		    SIPPYMODEM_V34_4D_INTERVALS_PER_MAPPING_FRAME ||
	    decoder->pending_interval_count >=
		    SIPPYMODEM_V34_4D_INTERVALS_PER_MAPPING_FRAME ||
	    !channel_frame_matches_schedule(decoder) ||
	    sippymodem_v34_mapping_parameters_init(
		    &expected, &decoder->channel_schedule.geometry,
		    decoder->channel_decoder.parameters.expanded_shaping) !=
		    SIPPYMODEM_V34_MAPPING_PARAMETERS_OK ||
	    !mapping_parameters_are_equal(
		    &decoder->channel_decoder.parameters, &expected) ||
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
	struct sippymodem_v34_channel_frame_decoder *decoder)
{
	if (sippymodem_v34_frame_schedule_is_complete(
		    &decoder->channel_schedule) &&
	    sippymodem_v34_frame_schedule_advance_data_frame(
		    &decoder->channel_schedule) !=
		    SIPPYMODEM_V34_FRAME_SCHEDULE_OK)
		return false;
	return sippymodem_v34_frame_schedule_next(
		       &decoder->channel_schedule, &decoder->channel_frame) ==
	       SIPPYMODEM_V34_FRAME_SCHEDULE_OK;
}

enum sippymodem_v34_channel_frame_decoder_result
sippymodem_v34_channel_frame_decoder_init(
	struct sippymodem_v34_channel_frame_decoder *decoder,
	const struct sippymodem_v34_frame_geometry *geometry,
	bool expanded_shaping,
	const struct sippymodem_v34_q14_coefficient
		coefficients[SIPPYMODEM_V34_PRECODER_TAP_COUNT],
	enum sippymodem_v34_trellis_selection selection)
{
	struct sippymodem_v34_channel_frame_decoder initialized = { 0 };
	struct sippymodem_v34_mapping_parameters parameters;

	if (decoder == NULL ||
	    sippymodem_v34_mapping_parameters_init(
		    &parameters, geometry, expanded_shaping) !=
		    SIPPYMODEM_V34_MAPPING_PARAMETERS_OK ||
	    sippymodem_v34_channel_decoder_init(
		    &initialized.channel_decoder, &parameters, coefficients,
		    selection) != SIPPYMODEM_V34_CHANNEL_DECODER_OK ||
	    sippymodem_v34_b1_frame_schedule_init(
		    &initialized.channel_schedule, geometry) !=
		    SIPPYMODEM_V34_FRAME_SCHEDULE_OK ||
	    !load_next_channel_frame(&initialized) ||
	    !sippymodem_v34_channel_frame_decoder_is_valid(&initialized))
		return SIPPYMODEM_V34_CHANNEL_FRAME_DECODER_INVALID_ARGUMENT;
	*decoder = initialized;
	return SIPPYMODEM_V34_CHANNEL_FRAME_DECODER_OK;
}

static enum sippymodem_v34_channel_frame_decoder_result channel_result(
	enum sippymodem_v34_channel_decoder_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_CHANNEL_DECODER_NUMERIC_FAILURE:
		return SIPPYMODEM_V34_CHANNEL_FRAME_DECODER_NUMERIC_FAILURE;
	case SIPPYMODEM_V34_CHANNEL_DECODER_OUT_OF_RANGE:
		return SIPPYMODEM_V34_CHANNEL_FRAME_DECODER_OUT_OF_RANGE;
	case SIPPYMODEM_V34_CHANNEL_DECODER_OK:
	case SIPPYMODEM_V34_CHANNEL_DECODER_INVALID_ARGUMENT:
		break;
	}
	return SIPPYMODEM_V34_CHANNEL_FRAME_DECODER_INVALID_ARGUMENT;
}

static bool advance_channel_cursor(
	struct sippymodem_v34_channel_frame_decoder *decoder)
{
	decoder->next_channel_interval++;
	if (decoder->next_channel_interval <
	    SIPPYMODEM_V34_4D_INTERVALS_PER_MAPPING_FRAME)
		return true;
	decoder->next_channel_interval = 0;
	return load_next_channel_frame(decoder);
}

enum sippymodem_v34_channel_frame_decoder_result
sippymodem_v34_channel_frame_decoder_advance(
	struct sippymodem_v34_channel_frame_decoder *decoder,
	const struct sippymodem_qam_point
		estimates[SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL],
	struct sippymodem_v34_survivor_decision *current,
	struct sippymodem_v34_decided_4d_interval
		decided_frame[SIPPYMODEM_V34_4D_INTERVALS_PER_MAPPING_FRAME],
	bool *frame_ready)
{
	struct sippymodem_v34_channel_frame_decoder next;
	struct sippymodem_v34_survivor_decision generated_current;
	struct sippymodem_v34_decided_4d_interval generated_decided;
	struct sippymodem_v34_decided_4d_interval generated_frame
		[SIPPYMODEM_V34_4D_INTERVALS_PER_MAPPING_FRAME];
	enum sippymodem_v34_channel_decoder_result result;
	uint8_t inversion_bit;
	bool decided_ready;
	bool generated_frame_ready = false;

	if (!sippymodem_v34_channel_frame_decoder_is_valid(decoder) ||
	    estimates == NULL || current == NULL || decided_frame == NULL ||
	    frame_ready == NULL)
		return SIPPYMODEM_V34_CHANNEL_FRAME_DECODER_INVALID_ARGUMENT;
	inversion_bit = (uint8_t)(
		(decoder->channel_frame.inversion_4d_mask >>
		 decoder->next_channel_interval) &
		1u);
	next = *decoder;
	result = sippymodem_v34_channel_decoder_advance(
		&next.channel_decoder, estimates, inversion_bit,
		&generated_current, &generated_decided, &decided_ready);
	if (result != SIPPYMODEM_V34_CHANNEL_DECODER_OK)
		return channel_result(result);
	if (!advance_channel_cursor(&next))
		return SIPPYMODEM_V34_CHANNEL_FRAME_DECODER_INVALID_ARGUMENT;
	if (decided_ready) {
		next.pending_frame[next.pending_interval_count++] =
			generated_decided;
		if (next.pending_interval_count ==
		    SIPPYMODEM_V34_4D_INTERVALS_PER_MAPPING_FRAME) {
			memcpy(generated_frame, next.pending_frame,
			       sizeof(generated_frame));
			next.pending_interval_count = 0;
			generated_frame_ready = true;
		}
	}
	if (!sippymodem_v34_channel_frame_decoder_is_valid(&next))
		return SIPPYMODEM_V34_CHANNEL_FRAME_DECODER_INVALID_ARGUMENT;
	*decoder = next;
	*current = generated_current;
	*frame_ready = generated_frame_ready;
	if (generated_frame_ready)
		memcpy(decided_frame, generated_frame, sizeof(generated_frame));
	return SIPPYMODEM_V34_CHANNEL_FRAME_DECODER_OK;
}

const char *sippymodem_v34_channel_frame_decoder_result_name(
	enum sippymodem_v34_channel_frame_decoder_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_CHANNEL_FRAME_DECODER_OK:
		return "ok";
	case SIPPYMODEM_V34_CHANNEL_FRAME_DECODER_INVALID_ARGUMENT:
		return "invalid argument";
	case SIPPYMODEM_V34_CHANNEL_FRAME_DECODER_NUMERIC_FAILURE:
		return "numeric failure";
	case SIPPYMODEM_V34_CHANNEL_FRAME_DECODER_OUT_OF_RANGE:
		return "out of range";
	}
	return "unknown";
}
