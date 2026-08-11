// SPDX-License-Identifier: BSD-3-Clause

#include "v34_data_channel_receiver.h"

#include <stddef.h>
#include <string.h>

static bool geometries_are_equal(
	const struct sippymodem_v34_frame_geometry *left,
	const struct sippymodem_v34_frame_geometry *right)
{
	return left->symbol_rate == right->symbol_rate &&
	       left->data_bits_per_frame == right->data_bits_per_frame &&
	       left->primary_data_rate_2400_bps_units ==
		       right->primary_data_rate_2400_bps_units &&
	       left->data_frames_per_superframe ==
		       right->data_frames_per_superframe &&
	       left->mapping_frames_per_data_frame ==
		       right->mapping_frames_per_data_frame &&
	       left->high_mapping_frame_bits == right->high_mapping_frame_bits &&
	       left->high_mapping_frame_count ==
		       right->high_mapping_frame_count &&
	       left->auxiliary_bits_per_frame ==
		       right->auxiliary_bits_per_frame &&
	       left->auxiliary_channel_enabled ==
		       right->auxiliary_channel_enabled;
}

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

bool sippymodem_v34_data_channel_receiver_is_valid(
	const struct sippymodem_v34_data_channel_receiver *receiver)
{
	const struct sippymodem_v34_channel_frame_decoder *channel_decoder;

	if (receiver == NULL ||
	    !sippymodem_v34_channel_frame_receiver_is_valid(
		    &receiver->channel_receiver) ||
	    !sippymodem_v34_data_receiver_is_valid(
		    &receiver->data_receiver))
		return false;
	channel_decoder = &receiver->channel_receiver.frame_decoder;
	return geometries_are_equal(
		       &channel_decoder->channel_schedule.geometry,
		       &receiver->data_receiver.schedule.geometry) &&
	       mapping_parameters_are_equal(
		       &channel_decoder->channel_decoder.parameters,
		       &receiver->data_receiver.mapping_decoder.parameters);
}

enum sippymodem_v34_data_channel_receiver_result
sippymodem_v34_data_channel_receiver_init(
	struct sippymodem_v34_data_channel_receiver *receiver,
	const struct sippymodem_v34_power_normalizer *normalizer,
	const struct sippymodem_v34_frame_geometry *geometry,
	bool expanded_shaping,
	const struct sippymodem_v34_q14_coefficient
		coefficients[SIPPYMODEM_V34_PRECODER_TAP_COUNT],
	enum sippymodem_v34_trellis_selection selection,
	enum sippymodem_v34_station_role transmitting_role)
{
	struct sippymodem_v34_data_channel_receiver initialized = { 0 };

	if (receiver == NULL ||
	    sippymodem_v34_channel_frame_receiver_init(
		    &initialized.channel_receiver, normalizer, geometry,
		    expanded_shaping, coefficients, selection) !=
		    SIPPYMODEM_V34_CHANNEL_FRAME_RECEIVER_OK ||
	    sippymodem_v34_data_receiver_init(
		    &initialized.data_receiver, geometry, expanded_shaping,
		    transmitting_role) != SIPPYMODEM_V34_DATA_RECEIVER_OK ||
	    !sippymodem_v34_data_channel_receiver_is_valid(&initialized))
		return SIPPYMODEM_V34_DATA_CHANNEL_RECEIVER_INVALID_ARGUMENT;
	*receiver = initialized;
	return SIPPYMODEM_V34_DATA_CHANNEL_RECEIVER_OK;
}

bool sippymodem_v34_data_channel_receiver_is_receiving_b1(
	const struct sippymodem_v34_data_channel_receiver *receiver)
{
	return sippymodem_v34_data_channel_receiver_is_valid(receiver) &&
	       sippymodem_v34_data_receiver_is_receiving_b1(
		       &receiver->data_receiver);
}

static enum sippymodem_v34_data_channel_receiver_result channel_result(
	enum sippymodem_v34_channel_frame_receiver_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_CHANNEL_FRAME_RECEIVER_NUMERIC_FAILURE:
		return SIPPYMODEM_V34_DATA_CHANNEL_RECEIVER_NUMERIC_FAILURE;
	case SIPPYMODEM_V34_CHANNEL_FRAME_RECEIVER_OUT_OF_RANGE:
		return SIPPYMODEM_V34_DATA_CHANNEL_RECEIVER_OUT_OF_RANGE;
	case SIPPYMODEM_V34_CHANNEL_FRAME_RECEIVER_OK:
	case SIPPYMODEM_V34_CHANNEL_FRAME_RECEIVER_INVALID_ARGUMENT:
		break;
	}
	return SIPPYMODEM_V34_DATA_CHANNEL_RECEIVER_INVALID_ARGUMENT;
}

static enum sippymodem_v34_data_channel_receiver_result data_result(
	enum sippymodem_v34_data_receiver_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_DATA_RECEIVER_OK:
		return SIPPYMODEM_V34_DATA_CHANNEL_RECEIVER_MAPPING_FRAME_READY;
	case SIPPYMODEM_V34_DATA_RECEIVER_B1_IN_PROGRESS:
		return SIPPYMODEM_V34_DATA_CHANNEL_RECEIVER_B1_IN_PROGRESS;
	case SIPPYMODEM_V34_DATA_RECEIVER_B1_COMPLETE:
		return SIPPYMODEM_V34_DATA_CHANNEL_RECEIVER_B1_COMPLETE;
	case SIPPYMODEM_V34_DATA_RECEIVER_INVALID_ARGUMENT:
		break;
	}
	return SIPPYMODEM_V34_DATA_CHANNEL_RECEIVER_INVALID_ARGUMENT;
}

enum sippymodem_v34_data_channel_receiver_result
sippymodem_v34_data_channel_receiver_advance(
	struct sippymodem_v34_data_channel_receiver *receiver,
	const struct sippymodem_qam_point
		normalized[SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL],
	struct sippymodem_v34_survivor_decision *current,
	struct sippymodem_qam_point
		decision_targets[SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL],
	struct sippymodem_v34_received_mapping_frame *received)
{
	struct sippymodem_v34_data_channel_receiver next;
	struct sippymodem_v34_survivor_decision generated_current;
	struct sippymodem_qam_point generated_targets
		[SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL];
	struct sippymodem_v34_decided_4d_interval decided
		[SIPPYMODEM_V34_4D_INTERVALS_PER_MAPPING_FRAME];
	struct sippymodem_v34_received_mapping_frame generated_received;
	enum sippymodem_v34_data_channel_receiver_result emitted_status;
	enum sippymodem_v34_channel_frame_receiver_result channel_status;
	enum sippymodem_v34_data_receiver_result data_status;
	bool frame_ready;

	if (!sippymodem_v34_data_channel_receiver_is_valid(receiver) ||
	    normalized == NULL || current == NULL || decision_targets == NULL ||
	    received == NULL)
		return SIPPYMODEM_V34_DATA_CHANNEL_RECEIVER_INVALID_ARGUMENT;
	next = *receiver;
	channel_status = sippymodem_v34_channel_frame_receiver_advance(
		&next.channel_receiver, normalized, &generated_current,
		generated_targets, decided, &frame_ready);
	if (channel_status != SIPPYMODEM_V34_CHANNEL_FRAME_RECEIVER_OK)
		return channel_result(channel_status);
	if (!frame_ready) {
		if (!sippymodem_v34_data_channel_receiver_is_valid(&next))
			return SIPPYMODEM_V34_DATA_CHANNEL_RECEIVER_INVALID_ARGUMENT;
		*receiver = next;
		*current = generated_current;
		memcpy(decision_targets, generated_targets,
		       sizeof(generated_targets));
		return SIPPYMODEM_V34_DATA_CHANNEL_RECEIVER_OK;
	}
	data_status = sippymodem_v34_data_receiver_receive_mapping_frame(
		&next.data_receiver, decided, &generated_received);
	emitted_status = data_result(data_status);
	if (emitted_status ==
		    SIPPYMODEM_V34_DATA_CHANNEL_RECEIVER_INVALID_ARGUMENT ||
	    !sippymodem_v34_data_channel_receiver_is_valid(&next))
		return SIPPYMODEM_V34_DATA_CHANNEL_RECEIVER_INVALID_ARGUMENT;
	*receiver = next;
	*current = generated_current;
	memcpy(decision_targets, generated_targets, sizeof(generated_targets));
	*received = generated_received;
	return emitted_status;
}

const char *sippymodem_v34_data_channel_receiver_result_name(
	enum sippymodem_v34_data_channel_receiver_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_DATA_CHANNEL_RECEIVER_OK:
		return "ok";
	case SIPPYMODEM_V34_DATA_CHANNEL_RECEIVER_MAPPING_FRAME_READY:
		return "mapping frame ready";
	case SIPPYMODEM_V34_DATA_CHANNEL_RECEIVER_B1_IN_PROGRESS:
		return "B1 in progress";
	case SIPPYMODEM_V34_DATA_CHANNEL_RECEIVER_B1_COMPLETE:
		return "B1 complete";
	case SIPPYMODEM_V34_DATA_CHANNEL_RECEIVER_INVALID_ARGUMENT:
		return "invalid argument";
	case SIPPYMODEM_V34_DATA_CHANNEL_RECEIVER_NUMERIC_FAILURE:
		return "numeric failure";
	case SIPPYMODEM_V34_DATA_CHANNEL_RECEIVER_OUT_OF_RANGE:
		return "out of range";
	}
	return "unknown";
}
