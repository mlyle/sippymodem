// SPDX-License-Identifier: BSD-3-Clause

#include "v34_data_transmitter.h"

#include <stddef.h>

static bool geometries_are_equal(
	const struct v34_frame_geometry *left,
	const struct v34_frame_geometry *right)
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
	       left->auxiliary_bits_per_frame == right->auxiliary_bits_per_frame &&
	       left->auxiliary_channel_enabled == right->auxiliary_channel_enabled;
}

static bool mapping_parameters_are_equal(
	const struct v34_mapping_parameters *left,
	const struct v34_mapping_parameters *right)
{
	return left->constellation_points == right->constellation_points &&
	       left->mapping_frame_bits == right->mapping_frame_bits &&
	       left->shell_mapper_bits == right->shell_mapper_bits &&
	       left->point_index_bits_per_2d_symbol ==
		       right->point_index_bits_per_2d_symbol &&
	       left->ring_count == right->ring_count &&
	       left->expanded_shaping == right->expanded_shaping;
}

static bool normalizers_are_equal(
	const struct v34_power_normalizer *left,
	const struct v34_power_normalizer *right)
{
	return mapping_parameters_are_equal(
		       &left->parameters, &right->parameters) &&
	       left->precoded_mean_squared_magnitude ==
		       right->precoded_mean_squared_magnitude &&
	       left->projected_mean_squared_magnitude ==
		       right->projected_mean_squared_magnitude &&
	       left->normalization_gain == right->normalization_gain &&
	       left->maximum_precoded_point_power ==
		       right->maximum_precoded_point_power &&
	       left->maximum_projected_point_power ==
		       right->maximum_projected_point_power &&
	       left->maximum_normalized_point_power ==
		       right->maximum_normalized_point_power &&
	       left->calibration_symbol_count ==
		       right->calibration_symbol_count &&
	       left->nonlinear_parameter == right->nonlinear_parameter;
}

static enum v34_data_transmitter_result line_result(
	enum v34_data_line_transmitter_result result)
{
	switch (result) {
	case V34_DATA_LINE_TRANSMITTER_OK:
		return V34_DATA_TRANSMITTER_OK;
	case V34_DATA_LINE_TRANSMITTER_OUT_OF_RANGE:
		return V34_DATA_TRANSMITTER_OUT_OF_RANGE;
	case V34_DATA_LINE_TRANSMITTER_CLIPPING_RISK:
		return V34_DATA_TRANSMITTER_CLIPPING_RISK;
	case V34_DATA_LINE_TRANSMITTER_NUMERIC_FAILURE:
		return V34_DATA_TRANSMITTER_NUMERIC_FAILURE;
	case V34_DATA_LINE_TRANSMITTER_INVALID_ARGUMENT:
		break;
	}
	return V34_DATA_TRANSMITTER_INVALID_ARGUMENT;
}

static enum v34_data_transmitter_result points_result(
	enum v34_data_points_result result)
{
	switch (result) {
	case V34_DATA_POINTS_OK:
		return V34_DATA_TRANSMITTER_OK;
	case V34_DATA_POINTS_MAPPING_FRAME_REQUIRED:
		return V34_DATA_TRANSMITTER_MAPPING_FRAME_REQUIRED;
	case V34_DATA_POINTS_MAPPING_FRAME_PENDING:
		return V34_DATA_TRANSMITTER_MAPPING_FRAME_PENDING;
	case V34_DATA_POINTS_INVALID_ARGUMENT:
		return V34_DATA_TRANSMITTER_INVALID_ARGUMENT;
	case V34_DATA_POINTS_OUT_OF_RANGE:
		return V34_DATA_TRANSMITTER_OUT_OF_RANGE;
	case V34_DATA_POINTS_NUMERIC_FAILURE:
		return V34_DATA_TRANSMITTER_NUMERIC_FAILURE;
	}
	return V34_DATA_TRANSMITTER_NUMERIC_FAILURE;
}

bool v34_data_transmitter_is_valid(
	const struct v34_data_transmitter *transmitter)
{
	return transmitter != NULL &&
	       v34_data_points_is_valid(&transmitter->points) &&
	       v34_data_line_transmitter_is_valid(&transmitter->line) &&
	       transmitter->points.local_role ==
		       transmitter->line.carrier.profile.local_role &&
	       geometries_are_equal(
		       &transmitter->points.schedule.geometry,
		       &transmitter->line.carrier.geometry) &&
	       normalizers_are_equal(
		       &transmitter->points.normalizer,
		       &transmitter->line.carrier.normalizer);
}

enum v34_data_transmitter_result v34_data_transmitter_init(
	struct v34_data_transmitter *transmitter,
	const struct v34_frame_schedule *initial_schedule,
	const struct v34_scrambler *initial_scrambler,
	const struct v34_4d_encoder *initial_mapping_encoder,
	const struct v34_power_normalizer *normalizer,
	const struct v34_phase3_profile *profile,
	uint32_t sample_rate, int16_t nominal_peak_amplitude)
{
	struct v34_data_transmitter initialized = { 0 };
	enum v34_data_transmitter_result result;

	if (transmitter == NULL || profile == NULL)
		return V34_DATA_TRANSMITTER_INVALID_ARGUMENT;
	result = points_result(v34_data_points_init(
		&initialized.points, initial_schedule, initial_scrambler,
		initial_mapping_encoder, normalizer, profile->local_role));
	if (result != V34_DATA_TRANSMITTER_OK)
		return result;
	result = line_result(v34_data_line_transmitter_init(
		&initialized.line, initial_schedule, initial_scrambler,
		initial_mapping_encoder, normalizer, profile, sample_rate,
		nominal_peak_amplitude));
	if (result != V34_DATA_TRANSMITTER_OK)
		return result;
	if (!v34_data_transmitter_is_valid(&initialized))
		return V34_DATA_TRANSMITTER_NUMERIC_FAILURE;
	*transmitter = initialized;
	return V34_DATA_TRANSMITTER_OK;
}

enum v34_data_transmitter_result v34_data_transmitter_continue(
	struct v34_data_transmitter *transmitter,
	const struct v34_training_line *active_line,
	const struct v34_frame_schedule *initial_schedule,
	const struct v34_scrambler *initial_scrambler,
	const struct v34_4d_encoder *initial_mapping_encoder,
	const struct v34_power_normalizer *normalizer)
{
	struct v34_data_transmitter continued = { 0 };
	enum v34_data_transmitter_result result;

	if (transmitter == NULL || !v34_training_line_is_valid(active_line))
		return V34_DATA_TRANSMITTER_INVALID_ARGUMENT;
	result = points_result(v34_data_points_init(
		&continued.points, initial_schedule, initial_scrambler,
		initial_mapping_encoder, normalizer,
		active_line->profile.local_role));
	if (result != V34_DATA_TRANSMITTER_OK)
		return result;
	result = line_result(v34_data_line_transmitter_continue(
		&continued.line, active_line, initial_schedule,
		initial_scrambler, initial_mapping_encoder, normalizer));
	if (result != V34_DATA_TRANSMITTER_OK)
		return result;
	if (!v34_data_transmitter_is_valid(&continued))
		return V34_DATA_TRANSMITTER_NUMERIC_FAILURE;
	*transmitter = continued;
	return V34_DATA_TRANSMITTER_OK;
}

bool v34_data_transmitter_needs_mapping_frame(
	const struct v34_data_transmitter *transmitter)
{
	return v34_data_transmitter_is_valid(transmitter) &&
	       v34_data_points_needs_mapping_frame(&transmitter->points);
}

enum v34_data_transmitter_result
v34_data_transmitter_preview_mapping_frame(
	const struct v34_data_transmitter *transmitter,
	struct v34_mapping_frame *mapping_frame)
{
	if (!v34_data_transmitter_is_valid(transmitter) ||
	    mapping_frame == NULL)
		return V34_DATA_TRANSMITTER_INVALID_ARGUMENT;
	return points_result(v34_data_points_preview_mapping_frame(
		&transmitter->points, mapping_frame));
}

enum v34_data_transmitter_result
v34_data_transmitter_queue_mapping_frame(
	struct v34_data_transmitter *transmitter,
	const uint8_t *primary_bits, size_t primary_bit_count,
	const uint8_t *auxiliary_bit)
{
	if (!v34_data_transmitter_is_valid(transmitter))
		return V34_DATA_TRANSMITTER_INVALID_ARGUMENT;
	return points_result(v34_data_points_queue_mapping_frame(
		&transmitter->points, primary_bits, primary_bit_count,
		auxiliary_bit));
}

enum v34_data_transmitter_result v34_data_transmitter_sample(
	struct v34_data_transmitter *transmitter,
	int16_t *sample, bool *symbol_accepted)
{
	struct v34_data_transmitter next;
	struct qam_point point;
	const struct qam_point *point_pointer = NULL;
	int16_t generated_sample;
	bool generated_accepted;
	enum v34_data_transmitter_result result;

	if (!v34_data_transmitter_is_valid(transmitter) || sample == NULL ||
	    symbol_accepted == NULL)
		return V34_DATA_TRANSMITTER_INVALID_ARGUMENT;
	next = *transmitter;
	if (v34_data_line_transmitter_needs_point(&next.line)) {
		result = points_result(v34_data_points_next(&next.points, &point));
		if (result != V34_DATA_TRANSMITTER_OK)
			return result;
		point_pointer = &point;
	}
	result = line_result(v34_data_line_transmitter_sample(
		&next.line, point_pointer, &generated_sample,
		&generated_accepted));
	if (result != V34_DATA_TRANSMITTER_OK)
		return result;
	if (!v34_data_transmitter_is_valid(&next))
		return V34_DATA_TRANSMITTER_NUMERIC_FAILURE;
	*transmitter = next;
	*sample = generated_sample;
	*symbol_accepted = generated_accepted;
	return V34_DATA_TRANSMITTER_OK;
}

const char *v34_data_transmitter_result_name(
	enum v34_data_transmitter_result result)
{
	switch (result) {
	case V34_DATA_TRANSMITTER_OK:
		return "ok";
	case V34_DATA_TRANSMITTER_MAPPING_FRAME_REQUIRED:
		return "mapping frame required";
	case V34_DATA_TRANSMITTER_MAPPING_FRAME_PENDING:
		return "mapping frame pending";
	case V34_DATA_TRANSMITTER_INVALID_ARGUMENT:
		return "invalid argument";
	case V34_DATA_TRANSMITTER_OUT_OF_RANGE:
		return "out of range";
	case V34_DATA_TRANSMITTER_CLIPPING_RISK:
		return "clipping risk";
	case V34_DATA_TRANSMITTER_NUMERIC_FAILURE:
		return "numeric failure";
	}
	return "unknown";
}
