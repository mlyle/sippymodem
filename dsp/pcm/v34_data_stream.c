// SPDX-License-Identifier: BSD-3-Clause

#include "v34_data_stream.h"

#include "packed_bits.h"
#include "v34_data_mapping.h"

#include <stddef.h>
#include <string.h>

static enum v34_data_stream_result transmitter_result(
	enum v34_data_transmitter_result result)
{
	switch (result) {
	case V34_DATA_TRANSMITTER_OK:
		return V34_DATA_STREAM_OK;
	case V34_DATA_TRANSMITTER_INVALID_ARGUMENT:
		return V34_DATA_STREAM_INVALID_ARGUMENT;
	case V34_DATA_TRANSMITTER_OUT_OF_RANGE:
		return V34_DATA_STREAM_OUT_OF_RANGE;
	case V34_DATA_TRANSMITTER_CLIPPING_RISK:
		return V34_DATA_STREAM_CLIPPING_RISK;
	case V34_DATA_TRANSMITTER_NUMERIC_FAILURE:
		return V34_DATA_STREAM_NUMERIC_FAILURE;
	case V34_DATA_TRANSMITTER_MAPPING_FRAME_REQUIRED:
	case V34_DATA_TRANSMITTER_MAPPING_FRAME_PENDING:
		break;
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

static bool profiles_are_equal(
	const struct v34_phase3_profile *left,
	const struct v34_phase3_profile *right)
{
	return left->symbol_rate_numerator == right->symbol_rate_numerator &&
	       left->symbol_rate_denominator == right->symbol_rate_denominator &&
	       left->carrier_frequency_numerator ==
		       right->carrier_frequency_numerator &&
	       left->carrier_frequency_denominator ==
		       right->carrier_frequency_denominator &&
	       left->md_symbol_count == right->md_symbol_count &&
	       left->symbol_rate == right->symbol_rate &&
	       left->local_role == right->local_role &&
	       left->transmit_power_reduction_db ==
		       right->transmit_power_reduction_db &&
	       left->preemphasis_filter_index ==
		       right->preemphasis_filter_index &&
	       left->projected_data_rate_2400_bps_units ==
		       right->projected_data_rate_2400_bps_units &&
	       left->md_length_35_ms_units == right->md_length_35_ms_units &&
	       left->high_carrier == right->high_carrier;
}

static bool configuration_matches_line(
	const struct v34_data_stream_configuration *configuration,
	const struct v34_training_line *line)
{
	return configuration != NULL && v34_training_line_is_valid(line) &&
	       profiles_are_equal(&configuration->profile, &line->profile) &&
	       configuration->sample_rate == line->carrier.channel.sample_rate &&
	       configuration->nominal_peak_amplitude ==
		       line->nominal_peak_amplitude;
}

enum v34_data_stream_result v34_data_stream_init(
	struct v34_data_stream *stream,
	const struct v34_data_stream_configuration *configuration)
{
	struct v34_data_mapping mapping;
	struct v34_data_transmitter transmitter;
	enum v34_data_stream_result result;

	if (stream == NULL)
		return V34_DATA_STREAM_INVALID_ARGUMENT;
	result = prepare_mapping(configuration, &mapping);
	if (result != V34_DATA_STREAM_OK)
		return result;
	result = transmitter_result(v34_data_transmitter_init(
		&transmitter, &mapping.schedule, &mapping.scrambler,
		&mapping.encoder, &mapping.normalizer,
		&configuration->profile, configuration->sample_rate,
		configuration->nominal_peak_amplitude));
	if (result != V34_DATA_STREAM_OK)
		return result;
	return v34_data_stream_init_b1(stream, &transmitter);
}

enum v34_data_stream_result v34_data_stream_continue_b1(
	struct v34_data_stream *stream,
	const struct v34_training_line *active_line,
	const struct v34_data_stream_configuration *configuration)
{
	struct v34_data_mapping mapping;
	enum v34_data_stream_result result;

	if (stream == NULL ||
	    !configuration_matches_line(configuration, active_line))
		return V34_DATA_STREAM_INVALID_ARGUMENT;
	result = prepare_mapping(configuration, &mapping);
	if (result != V34_DATA_STREAM_OK)
		return result;
	return v34_data_stream_continue_prepared_b1(
		stream, active_line, &mapping);
}

enum v34_data_stream_result v34_data_stream_continue_prepared_b1(
	struct v34_data_stream *stream,
	const struct v34_training_line *active_line,
	const struct v34_data_mapping *mapping)
{
	struct v34_data_transmitter transmitter;
	enum v34_data_stream_result result;

	if (stream == NULL || !v34_data_mapping_is_valid(mapping) ||
	    !v34_training_line_is_valid(active_line) ||
	    !profiles_are_equal(
		    &mapping->transmitting_profile,
		    &active_line->profile))
		return V34_DATA_STREAM_INVALID_ARGUMENT;
	result = transmitter_result(v34_data_transmitter_continue(
		&transmitter, active_line, &mapping->schedule,
		&mapping->scrambler, &mapping->encoder,
		&mapping->normalizer));
	if (result != V34_DATA_STREAM_OK)
		return result;
	return v34_data_stream_init_b1(stream, &transmitter);
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
	       v34_data_transmitter_is_valid(
		       &stream->transmitter) &&
	       primary_delay_is_valid(
		       stream->delayed_primary_bits,
		       stream->primary_delay_bits) &&
	       (!stream->b1_active ||
		stream->transmitter.points.schedule.data_frame_in_superframe + 1u ==
			stream->transmitter.points.schedule.geometry
				.data_frames_per_superframe);
}

enum v34_data_stream_result v34_data_stream_init_b1(
	struct v34_data_stream *stream,
	const struct v34_data_transmitter *transmitter)
{
	struct v34_data_stream initialized;
	uint8_t delay_bits;

	if (stream == NULL ||
	    !v34_data_transmitter_is_valid(transmitter) ||
	    !v34_data_transmitter_needs_mapping_frame(transmitter) ||
	    !schedule_is_at_b1_start(&transmitter->points.schedule) ||
	    !b1_primary_delay(
		    &transmitter->points.schedule,
		    &transmitter->points.encoder.parameters,
		    &delay_bits))
		return V34_DATA_STREAM_INVALID_ARGUMENT;
	initialized = (struct v34_data_stream){
		.transmitter = *transmitter,
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
	enum v34_data_stream_result result;

	if (!v34_data_stream_is_valid(stream) || sample == NULL ||
	    symbol_accepted == NULL)
		return V34_DATA_STREAM_INVALID_ARGUMENT;
	if (!v34_data_transmitter_needs_mapping_frame(
		    &stream->transmitter))
		return transmitter_result(v34_data_transmitter_sample(
			&stream->transmitter, sample, symbol_accepted));

	next = *stream;
	if (next.b1_active && v34_frame_schedule_is_complete(
				      &next.transmitter.points.schedule))
		next.b1_active = false;
	result = transmitter_result(
		v34_data_transmitter_preview_mapping_frame(
			&next.transmitter, &request));
	if (result != V34_DATA_STREAM_OK)
		return result;
	if (!acquire_mapping_frame(
		    &next, source, source_context, &request, primary_bits,
		    &auxiliary_bit))
		return V34_DATA_STREAM_SOURCE_FAILURE;
	auxiliary = request.carries_auxiliary_bit ? &auxiliary_bit : NULL;
	result = transmitter_result(
		v34_data_transmitter_queue_mapping_frame(
			&next.transmitter, primary_bits, request.primary_bits,
			auxiliary));
	if (result != V34_DATA_STREAM_OK)
		return result;
	result = transmitter_result(v34_data_transmitter_sample(
		&next.transmitter, &generated_sample, &generated_accepted));
	if (result != V34_DATA_STREAM_OK)
		return result;
	if (!v34_data_stream_is_valid(&next))
		return V34_DATA_STREAM_NUMERIC_FAILURE;
	*stream = next;
	*sample = generated_sample;
	*symbol_accepted = generated_accepted;
	return V34_DATA_STREAM_OK;
}

const char *v34_data_stream_result_name(
	enum v34_data_stream_result result)
{
	switch (result) {
	case V34_DATA_STREAM_OK:
		return "ok";
	case V34_DATA_STREAM_SOURCE_FAILURE:
		return "mapping-frame source failure";
	case V34_DATA_STREAM_INVALID_ARGUMENT:
		return "invalid argument";
	case V34_DATA_STREAM_OUT_OF_RANGE:
		return "out of range";
	case V34_DATA_STREAM_CLIPPING_RISK:
		return "clipping risk";
	case V34_DATA_STREAM_NUMERIC_FAILURE:
		return "numeric failure";
	}
	return "unknown";
}
