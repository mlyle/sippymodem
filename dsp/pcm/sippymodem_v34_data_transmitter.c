// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_v34_data_transmitter.h"

#include <stddef.h>

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
	       left->high_mapping_frame_bits ==
		       right->high_mapping_frame_bits &&
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

static enum sippymodem_v34_data_transmitter_result line_result(
	enum sippymodem_v34_data_line_transmitter_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_OK:
		return SIPPYMODEM_V34_DATA_TRANSMITTER_OK;
	case SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_MAPPING_FRAME_REQUIRED:
		return SIPPYMODEM_V34_DATA_TRANSMITTER_MAPPING_FRAME_REQUIRED;
	case SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_MAPPING_FRAME_PENDING:
		return SIPPYMODEM_V34_DATA_TRANSMITTER_MAPPING_FRAME_PENDING;
	case SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_OUT_OF_RANGE:
		return SIPPYMODEM_V34_DATA_TRANSMITTER_OUT_OF_RANGE;
	case SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_CLIPPING_RISK:
		return SIPPYMODEM_V34_DATA_TRANSMITTER_CLIPPING_RISK;
	case SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_NUMERIC_FAILURE:
		return SIPPYMODEM_V34_DATA_TRANSMITTER_NUMERIC_FAILURE;
	case SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_INVALID_ARGUMENT:
		break;
	}
	return SIPPYMODEM_V34_DATA_TRANSMITTER_INVALID_ARGUMENT;
}

static enum sippymodem_v34_data_transmitter_result mapping_result(
	enum sippymodem_v34_mapping_frame_transmitter_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_MAPPING_FRAME_TRANSMITTER_OK:
		return SIPPYMODEM_V34_DATA_TRANSMITTER_OK;
	case SIPPYMODEM_V34_MAPPING_FRAME_TRANSMITTER_INVALID_ARGUMENT:
		return SIPPYMODEM_V34_DATA_TRANSMITTER_INVALID_ARGUMENT;
	case SIPPYMODEM_V34_MAPPING_FRAME_TRANSMITTER_OUT_OF_RANGE:
		return SIPPYMODEM_V34_DATA_TRANSMITTER_OUT_OF_RANGE;
	case SIPPYMODEM_V34_MAPPING_FRAME_TRANSMITTER_NUMERIC_FAILURE:
		return SIPPYMODEM_V34_DATA_TRANSMITTER_NUMERIC_FAILURE;
	case SIPPYMODEM_V34_MAPPING_FRAME_TRANSMITTER_FRAME_COMPLETE:
		break;
	}
	return SIPPYMODEM_V34_DATA_TRANSMITTER_NUMERIC_FAILURE;
}

static bool prepare_next_schedule(
	struct sippymodem_v34_frame_schedule *schedule)
{
	return !sippymodem_v34_frame_schedule_is_complete(schedule) ||
	       sippymodem_v34_frame_schedule_advance_data_frame(schedule) ==
		       SIPPYMODEM_V34_FRAME_SCHEDULE_OK;
}

bool sippymodem_v34_data_transmitter_is_valid(
	const struct sippymodem_v34_data_transmitter *transmitter)
{
	return transmitter != NULL &&
	       sippymodem_v34_frame_schedule_is_valid(&transmitter->schedule) &&
	       sippymodem_v34_data_line_transmitter_is_valid(
		       &transmitter->line) &&
	       sippymodem_v34_transmit_scrambler_is_valid(
		       &transmitter->scrambler,
		       transmitter->line.carrier.profile.local_role) &&
	       sippymodem_v34_4d_encoder_is_valid(
		       &transmitter->mapping_encoder) &&
	       geometries_are_equal(
		       &transmitter->schedule.geometry,
		       &transmitter->line.carrier.geometry) &&
	       mapping_parameters_are_equal(
		       &transmitter->mapping_encoder.parameters,
		       &transmitter->line.carrier.normalizer.parameters);
}

enum sippymodem_v34_data_transmitter_result
sippymodem_v34_data_transmitter_init(
	struct sippymodem_v34_data_transmitter *transmitter,
	const struct sippymodem_v34_frame_schedule *initial_schedule,
	const struct sippymodem_v34_scrambler *initial_scrambler,
	const struct sippymodem_v34_4d_encoder *initial_mapping_encoder,
	const struct sippymodem_v34_power_normalizer *normalizer,
	const struct sippymodem_v34_phase3_profile *profile,
	uint32_t sample_rate, int16_t nominal_peak_amplitude)
{
	struct sippymodem_v34_data_transmitter initialized = { 0 };
	enum sippymodem_v34_data_transmitter_result result;

	if (transmitter == NULL)
		return SIPPYMODEM_V34_DATA_TRANSMITTER_INVALID_ARGUMENT;
	result = line_result(sippymodem_v34_data_line_transmitter_init(
		&initialized.line, initial_schedule, initial_scrambler,
		initial_mapping_encoder, normalizer, profile, sample_rate,
		nominal_peak_amplitude));
	if (result != SIPPYMODEM_V34_DATA_TRANSMITTER_OK)
		return result;
	initialized.schedule = *initial_schedule;
	initialized.scrambler = *initial_scrambler;
	initialized.mapping_encoder = *initial_mapping_encoder;
	if (!sippymodem_v34_data_transmitter_is_valid(&initialized))
		return SIPPYMODEM_V34_DATA_TRANSMITTER_NUMERIC_FAILURE;
	*transmitter = initialized;
	return SIPPYMODEM_V34_DATA_TRANSMITTER_OK;
}

bool sippymodem_v34_data_transmitter_needs_mapping_frame(
	const struct sippymodem_v34_data_transmitter *transmitter)
{
	return sippymodem_v34_data_transmitter_is_valid(transmitter) &&
	       sippymodem_v34_data_line_transmitter_needs_mapping_frame(
		       &transmitter->line);
}

enum sippymodem_v34_data_transmitter_result
sippymodem_v34_data_transmitter_preview_mapping_frame(
	const struct sippymodem_v34_data_transmitter *transmitter,
	struct sippymodem_v34_mapping_frame *mapping_frame)
{
	struct sippymodem_v34_frame_schedule schedule;
	struct sippymodem_v34_mapping_frame preview;

	if (!sippymodem_v34_data_transmitter_is_valid(transmitter) ||
	    mapping_frame == NULL)
		return SIPPYMODEM_V34_DATA_TRANSMITTER_INVALID_ARGUMENT;
	if (!sippymodem_v34_data_line_transmitter_needs_mapping_frame(
		    &transmitter->line))
		return SIPPYMODEM_V34_DATA_TRANSMITTER_MAPPING_FRAME_PENDING;
	schedule = transmitter->schedule;
	if (!prepare_next_schedule(&schedule) ||
	    sippymodem_v34_frame_schedule_next(&schedule, &preview) !=
		    SIPPYMODEM_V34_FRAME_SCHEDULE_OK)
		return SIPPYMODEM_V34_DATA_TRANSMITTER_NUMERIC_FAILURE;
	*mapping_frame = preview;
	return SIPPYMODEM_V34_DATA_TRANSMITTER_OK;
}

enum sippymodem_v34_data_transmitter_result
sippymodem_v34_data_transmitter_queue_mapping_frame(
	struct sippymodem_v34_data_transmitter *transmitter,
	const uint8_t *primary_bits, size_t primary_bit_count,
	const uint8_t *auxiliary_bit)
{
	struct sippymodem_v34_data_transmitter next;
	struct sippymodem_v34_transmitted_mapping_frame frame;
	enum sippymodem_v34_data_transmitter_result result;

	if (!sippymodem_v34_data_transmitter_is_valid(transmitter))
		return SIPPYMODEM_V34_DATA_TRANSMITTER_INVALID_ARGUMENT;
	if (!sippymodem_v34_data_line_transmitter_needs_mapping_frame(
		    &transmitter->line))
		return SIPPYMODEM_V34_DATA_TRANSMITTER_MAPPING_FRAME_PENDING;
	next = *transmitter;
	if (!prepare_next_schedule(&next.schedule))
		return SIPPYMODEM_V34_DATA_TRANSMITTER_NUMERIC_FAILURE;
	result = mapping_result(sippymodem_v34_transmit_mapping_frame(
		&next.schedule, &next.scrambler, &next.mapping_encoder,
		primary_bits, primary_bit_count, auxiliary_bit,
		next.line.carrier.normalizer.nonlinear_parameter,
		next.line.carrier.normalizer.precoded_mean_squared_magnitude,
		&frame));
	if (result != SIPPYMODEM_V34_DATA_TRANSMITTER_OK)
		return result;
	result = line_result(
		sippymodem_v34_data_line_transmitter_load_mapping_frame(
			&next.line, &frame));
	if (result != SIPPYMODEM_V34_DATA_TRANSMITTER_OK)
		return result;
	if (!sippymodem_v34_data_transmitter_is_valid(&next))
		return SIPPYMODEM_V34_DATA_TRANSMITTER_NUMERIC_FAILURE;
	*transmitter = next;
	return SIPPYMODEM_V34_DATA_TRANSMITTER_OK;
}

enum sippymodem_v34_data_transmitter_result
sippymodem_v34_data_transmitter_sample(
	struct sippymodem_v34_data_transmitter *transmitter,
	int16_t *sample, bool *symbol_accepted)
{
	if (!sippymodem_v34_data_transmitter_is_valid(transmitter))
		return SIPPYMODEM_V34_DATA_TRANSMITTER_INVALID_ARGUMENT;
	return line_result(sippymodem_v34_data_line_transmitter_sample(
		&transmitter->line, sample, symbol_accepted));
}

const char *sippymodem_v34_data_transmitter_result_name(
	enum sippymodem_v34_data_transmitter_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_DATA_TRANSMITTER_OK:
		return "ok";
	case SIPPYMODEM_V34_DATA_TRANSMITTER_MAPPING_FRAME_REQUIRED:
		return "mapping frame required";
	case SIPPYMODEM_V34_DATA_TRANSMITTER_MAPPING_FRAME_PENDING:
		return "mapping frame pending";
	case SIPPYMODEM_V34_DATA_TRANSMITTER_INVALID_ARGUMENT:
		return "invalid argument";
	case SIPPYMODEM_V34_DATA_TRANSMITTER_OUT_OF_RANGE:
		return "out of range";
	case SIPPYMODEM_V34_DATA_TRANSMITTER_CLIPPING_RISK:
		return "clipping risk";
	case SIPPYMODEM_V34_DATA_TRANSMITTER_NUMERIC_FAILURE:
		return "numeric failure";
	}
	return "unknown";
}
