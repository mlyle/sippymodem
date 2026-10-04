// SPDX-License-Identifier: BSD-3-Clause

#include "v34_data_stream.h"

#include "packed_bits.h"
#include "v34_data_mapping.h"
#include "v34_power_calibration.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

static enum v34_data_stream_result points_result(
	enum v34_data_points_result result)
{
	switch (result) {
	case V34_DATA_POINTS_OK:
		return V34_DATA_STREAM_OK;
	case V34_DATA_POINTS_INVALID_ARGUMENT:
		return V34_DATA_STREAM_INVALID_ARGUMENT;
	case V34_DATA_POINTS_OUT_OF_RANGE:
		return V34_DATA_STREAM_OUT_OF_RANGE;
	case V34_DATA_POINTS_NUMERIC_FAILURE:
		return V34_DATA_STREAM_NUMERIC_FAILURE;
	case V34_DATA_POINTS_MAPPING_FRAME_REQUIRED:
	case V34_DATA_POINTS_MAPPING_FRAME_PENDING:
		break;
	}
	return V34_DATA_STREAM_NUMERIC_FAILURE;
}

static enum v34_data_stream_result line_result(
	enum v34_training_line_result result)
{
	switch (result) {
	case V34_TRAINING_LINE_OK:
		return V34_DATA_STREAM_OK;
	case V34_TRAINING_LINE_INVALID_ARGUMENT:
		return V34_DATA_STREAM_INVALID_ARGUMENT;
	case V34_TRAINING_LINE_OUT_OF_RANGE:
		return V34_DATA_STREAM_OUT_OF_RANGE;
	case V34_TRAINING_LINE_CLIPPING_RISK:
		return V34_DATA_STREAM_CLIPPING_RISK;
	case V34_TRAINING_LINE_NUMERIC_FAILURE:
		return V34_DATA_STREAM_NUMERIC_FAILURE;
	}
	return V34_DATA_STREAM_NUMERIC_FAILURE;
}

static enum v34_data_stream_result calibration_result(
	enum v34_power_calibration_result result)
{
	switch (result) {
	case V34_POWER_CALIBRATION_OK:
		return V34_DATA_STREAM_OK;
	case V34_POWER_CALIBRATION_INVALID_ARGUMENT:
		return V34_DATA_STREAM_INVALID_ARGUMENT;
	case V34_POWER_CALIBRATION_OUT_OF_RANGE:
		return V34_DATA_STREAM_OUT_OF_RANGE;
	case V34_POWER_CALIBRATION_IN_PROGRESS:
	case V34_POWER_CALIBRATION_NUMERIC_FAILURE:
		return V34_DATA_STREAM_NUMERIC_FAILURE;
	}
	return V34_DATA_STREAM_NUMERIC_FAILURE;
}

static enum v34_data_stream_result mapping_result(
	enum v34_data_mapping_result result)
{
	switch (result) {
	case V34_DATA_MAPPING_OK:
		return V34_DATA_STREAM_OK;
	case V34_DATA_MAPPING_INVALID_ARGUMENT:
		return V34_DATA_STREAM_INVALID_ARGUMENT;
	case V34_DATA_MAPPING_OUT_OF_RANGE:
		return V34_DATA_STREAM_OUT_OF_RANGE;
	case V34_DATA_MAPPING_NUMERIC_FAILURE:
		return V34_DATA_STREAM_NUMERIC_FAILURE;
	}
	return V34_DATA_STREAM_NUMERIC_FAILURE;
}

static enum v34_data_stream_result prepare_mapping(
	const struct v34_data_stream_configuration *configuration,
	struct v34_data_mapping *mapping)
{
	struct v34_data_direction_parameters parameters = { 0 };

	if (configuration == NULL || mapping == NULL ||
	    configuration->sample_rate == 0 ||
	    configuration->nominal_peak_amplitude <= 0)
		return V34_DATA_STREAM_INVALID_ARGUMENT;
	memcpy(parameters.precoder_coefficients,
	       configuration->precoder_coefficients,
	       sizeof(parameters.precoder_coefficients));
	parameters.trellis_selection = configuration->trellis_selection;
	parameters.nonlinear_parameter = configuration->nonlinear_parameter;
	parameters.primary_data_rate_2400_bps_units =
		configuration->primary_data_rate_2400_bps_units;
	parameters.expanded_shaping = configuration->expanded_shaping;
	return mapping_result(v34_data_mapping_prepare(
		mapping, &configuration->profile, &parameters,
		configuration->auxiliary_channel_enabled));
}

static bool configuration_matches_line(
	const struct v34_data_stream_configuration *configuration,
	const struct v34_training_line *line)
{
	return configuration != NULL && v34_training_line_is_valid(line) &&
	       v34_phase3_profile_is_equal(
		       &configuration->profile, &line->profile) &&
	       configuration->sample_rate == line->carrier.channel.sample_rate &&
	       configuration->nominal_peak_amplitude ==
		       line->nominal_peak_amplitude;
}

static enum v34_data_stream_result initialize_b1(
	struct v34_data_stream *stream,
	const struct v34_data_points *points,
	const struct v34_training_line *line);

enum v34_data_stream_result v34_data_stream_continue_b1(
	struct v34_data_stream *stream,
	const struct v34_training_line *active_line,
	const struct v34_data_stream_configuration *configuration)
{
	struct v34_data_mapping mapping;
	struct v34_data_stream_preparation preparation;
	enum v34_data_stream_result result;

	if (stream == NULL ||
	    !configuration_matches_line(configuration, active_line))
		return V34_DATA_STREAM_INVALID_ARGUMENT;
	result = prepare_mapping(configuration, &mapping);
	if (result != V34_DATA_STREAM_OK)
		return result;
	result = v34_data_stream_prepare(
		&preparation, &mapping, configuration->sample_rate);
	if (result != V34_DATA_STREAM_OK)
		return result;
	return v34_data_stream_continue_prepared_b1(
		stream, active_line, &preparation);
}

bool v34_data_stream_preparation_is_valid(
	const struct v34_data_stream_preparation *preparation)
{
	return preparation != NULL &&
	       v34_data_mapping_is_valid(&preparation->mapping) &&
	       preparation->sample_rate != 0 &&
	       isfinite(preparation->line_peak) && preparation->line_peak > 0.0;
}

enum v34_data_stream_result v34_data_stream_prepare(
	struct v34_data_stream_preparation *preparation,
	const struct v34_data_mapping *mapping, uint32_t sample_rate)
{
	struct v34_line_power_measurement measurement;
	enum v34_data_stream_result result;

	if (preparation == NULL || !v34_data_mapping_is_valid(mapping))
		return V34_DATA_STREAM_INVALID_ARGUMENT;
	result = calibration_result(v34_calibrate_line_power(
		&mapping->schedule, &mapping->scrambler, &mapping->encoder,
		&mapping->normalizer, &mapping->transmitting_profile,
		sample_rate, &measurement));
	if (result != V34_DATA_STREAM_OK)
		return result;
	*preparation = (struct v34_data_stream_preparation){
		.mapping = *mapping,
		.line_peak = measurement.peak_amplitude,
		.sample_rate = sample_rate,
	};
	return V34_DATA_STREAM_OK;
}

enum v34_data_stream_result v34_data_stream_continue_prepared_b1(
	struct v34_data_stream *stream,
	const struct v34_training_line *active_line,
	const struct v34_data_stream_preparation *preparation)
{
	struct v34_data_points points;
	struct v34_training_line line;
	const struct v34_data_mapping *mapping;
	enum v34_data_stream_result result;

	if (stream == NULL || !v34_data_stream_preparation_is_valid(preparation) ||
	    !v34_training_line_is_valid(active_line) ||
	    preparation->sample_rate != active_line->carrier.channel.sample_rate ||
	    !v34_phase3_profile_is_equal(
		    &preparation->mapping.transmitting_profile,
		    &active_line->profile))
		return V34_DATA_STREAM_INVALID_ARGUMENT;
	mapping = &preparation->mapping;

	result = points_result(v34_data_points_init(
		&points, &mapping->schedule, &mapping->scrambler,
		&mapping->encoder, &mapping->normalizer,
		active_line->profile.local_role));
	if (result != V34_DATA_STREAM_OK)
		return result;
	if (preparation->line_peak * active_line->output_gain >=
	    INT16_MAX + 0.5)
		return V34_DATA_STREAM_CLIPPING_RISK;
	line = *active_line;
	if (line.carrier.maximum_point_power <
	    mapping->normalizer.maximum_normalized_point_power)
		line.carrier.maximum_point_power =
			mapping->normalizer.maximum_normalized_point_power;
	if (!v34_training_line_is_valid(&line))
		return V34_DATA_STREAM_NUMERIC_FAILURE;
	return initialize_b1(stream, &points, &line);
}

static bool schedule_is_at_b1_start(
	const struct v34_frame_schedule *schedule)
{
	return v34_frame_schedule_is_valid(schedule) &&
	       schedule->data_frame_in_superframe + 1u ==
		       schedule->geometry.data_frames_per_superframe &&
	       schedule->next_mapping_frame == 0;
}

static bool b1_primary_delay(
	const struct v34_frame_schedule *initial,
	const struct v34_mapping_parameters *parameters,
	uint8_t *delay_bits)
{
	struct v34_frame_schedule schedule;
	int cursor = 32;

	if (!schedule_is_at_b1_start(initial) ||
	    !v34_mapping_parameters_is_valid(parameters) ||
	    parameters->mapping_frame_bits !=
		    initial->geometry.high_mapping_frame_bits ||
	    delay_bits == NULL)
		return false;
	schedule = *initial;
	while (!v34_frame_schedule_is_complete(&schedule)) {
		struct v34_mapping_frame frame;
		int first_bits;
		int final_differential_bits = 2;
		int final_step;

		if (v34_frame_schedule_next(&schedule, &frame) !=
		    V34_FRAME_SCHEDULE_OK)
			return false;
		first_bits = (int)parameters->shell_mapper_bits -
			(frame.high_frame ? 0 : 1);
		if (first_bits < 1)
			final_differential_bits =
				2 - (initial->geometry.high_mapping_frame_bits < 9u) +
				first_bits;
		final_step = final_differential_bits + 1 +
			2 * parameters->point_index_bits_per_2d_symbol;
		if (final_step <= 0 || frame.primary_bits < final_step)
			return false;
		/* Word fetches normalize the cursor before each field.  Only the
		 * final field can leave it beyond the current 16-bit word. */
		cursor = (cursor + frame.primary_bits - final_step) & 15;
		cursor += final_step;
	}
	if (cursor <= 0 || cursor >= 32)
		return false;
	*delay_bits = (uint8_t)(32 - cursor);
	return true;
}

static bool primary_delay_is_valid(
	uint32_t delayed_primary_bits, uint8_t delay_bits)
{
	return delay_bits > 0 && delay_bits < 32 &&
	       (delayed_primary_bits >> delay_bits) == 0;
}

bool v34_data_stream_is_valid(
	const struct v34_data_stream *stream)
{
	return stream != NULL &&
	       v34_data_points_is_valid(&stream->points) &&
	       v34_training_line_is_valid(&stream->line) &&
	       stream->points.local_role ==
		       stream->line.profile.local_role &&
	       stream->points.schedule.geometry.symbol_rate ==
		       stream->line.profile.symbol_rate &&
	       stream->line.carrier.maximum_point_power >=
		       stream->points.normalizer
			       .maximum_normalized_point_power &&
	       primary_delay_is_valid(
		       stream->delayed_primary_bits,
		       stream->primary_delay_bits) &&
	       (!stream->b1_active ||
		stream->points.schedule.data_frame_in_superframe + 1u ==
			stream->points.schedule.geometry
				.data_frames_per_superframe);
}

static enum v34_data_stream_result initialize_b1(
	struct v34_data_stream *stream,
	const struct v34_data_points *points,
	const struct v34_training_line *line)
{
	struct v34_data_stream initialized;
	uint8_t delay_bits;

	if (stream == NULL || !v34_data_points_is_valid(points) ||
	    !v34_training_line_is_valid(line) ||
	    !v34_data_points_needs_mapping_frame(points) ||
	    !schedule_is_at_b1_start(&points->schedule) ||
	    !b1_primary_delay(
		    &points->schedule, &points->encoder.parameters,
		    &delay_bits))
		return V34_DATA_STREAM_INVALID_ARGUMENT;
	initialized = (struct v34_data_stream){
		.points = *points,
		.line = *line,
		.delayed_primary_bits =
			UINT32_MAX >> (32u - delay_bits),
		.primary_delay_bits = delay_bits,
		.b1_active = true,
	};
	if (!v34_data_stream_is_valid(&initialized))
		return V34_DATA_STREAM_NUMERIC_FAILURE;
	*stream = initialized;
	return V34_DATA_STREAM_OK;
}

bool v34_data_stream_is_transmitting_b1(
	const struct v34_data_stream *stream)
{
	return v34_data_stream_is_valid(stream) &&
	       stream->b1_active;
}

static bool acquire_mapping_frame(
	struct v34_data_stream *stream,
	v34_mapping_frame_source source, void *source_context,
	const struct v34_mapping_frame *request,
	uint8_t primary_bits[V34_MAX_MAPPING_FRAME_BITS],
	uint8_t *auxiliary_bit)
{
	uint8_t sourced_primary_bits[V34_MAX_MAPPING_FRAME_BITS];
	uint8_t *auxiliary = request->carries_auxiliary_bit ?
		auxiliary_bit : NULL;

	if (stream->b1_active || source == NULL) {
		memset(sourced_primary_bits, 1, request->primary_bits);
		if (auxiliary != NULL)
			*auxiliary = 1;
	} else {
		memset(sourced_primary_bits, 2, request->primary_bits);
		if (auxiliary != NULL)
			*auxiliary = 2;
		if (!source(source_context, request, sourced_primary_bits,
			    auxiliary) ||
		    !unpacked_bits_are_binary(
			    sourced_primary_bits, request->primary_bits) ||
		    (auxiliary != NULL && *auxiliary > 1u))
			return false;
	}
	for (size_t bit = 0; bit < request->primary_bits; ++bit) {
		primary_bits[bit] = (uint8_t)(stream->delayed_primary_bits & 1u);
		stream->delayed_primary_bits =
			(stream->delayed_primary_bits >> 1) |
			((uint32_t)sourced_primary_bits[bit] <<
			 (stream->primary_delay_bits - 1u));
	}
	return true;
}

enum v34_data_stream_result v34_data_stream_sample(
	struct v34_data_stream *stream,
	v34_mapping_frame_source source, void *source_context,
	int16_t *sample, bool *symbol_accepted)
{
	struct v34_data_stream next;
	struct v34_mapping_frame request;
	uint8_t primary_bits[V34_MAX_MAPPING_FRAME_BITS];
	uint8_t auxiliary_bit;
	const uint8_t *auxiliary;
	int16_t generated_sample;
	bool generated_accepted;
	struct qam_point point;
	const struct qam_point *point_pointer = NULL;
	enum v34_data_stream_result result;

	if (!v34_data_stream_is_valid(stream) || sample == NULL ||
	    symbol_accepted == NULL)
		return V34_DATA_STREAM_INVALID_ARGUMENT;
	next = *stream;
	if (v34_data_points_needs_mapping_frame(&next.points)) {
		if (next.b1_active &&
		    v34_frame_schedule_is_complete(&next.points.schedule))
			next.b1_active = false;
		result = points_result(v34_data_points_preview_mapping_frame(
			&next.points, &request));
		if (result != V34_DATA_STREAM_OK)
			return result;
		if (!acquire_mapping_frame(
			    &next, source, source_context, &request, primary_bits,
			    &auxiliary_bit))
			return V34_DATA_STREAM_SOURCE_FAILURE;
		auxiliary = request.carries_auxiliary_bit ?
			&auxiliary_bit : NULL;
		result = points_result(v34_data_points_queue_mapping_frame(
			&next.points, primary_bits, request.primary_bits,
			auxiliary));
		if (result != V34_DATA_STREAM_OK)
			return result;
	}
	if (v34_training_line_needs_point(&next.line)) {
		result = points_result(v34_data_points_next(
			&next.points, &point));
		if (result != V34_DATA_STREAM_OK)
			return result;
		point_pointer = &point;
	}
	result = line_result(v34_training_line_sample(
		&next.line, point_pointer, &generated_sample,
		&generated_accepted));
	if (result != V34_DATA_STREAM_OK)
		return result;
	if (!v34_data_stream_is_valid(&next))
		return V34_DATA_STREAM_NUMERIC_FAILURE;
	*stream = next;
	*sample = generated_sample;
	*symbol_accepted = generated_accepted;
	return V34_DATA_STREAM_OK;
}
