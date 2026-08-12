// SPDX-License-Identifier: BSD-3-Clause

#include "v34_data_stream.h"

#include "v34_mapping_parameters.h"

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

static enum v34_data_stream_result geometry_result(
	enum v34_frame_schedule_result result)
{
	switch (result) {
	case V34_FRAME_SCHEDULE_OK:
		return V34_DATA_STREAM_OK;
	case V34_FRAME_SCHEDULE_INVALID_ARGUMENT:
		return V34_DATA_STREAM_INVALID_ARGUMENT;
	case V34_FRAME_SCHEDULE_UNSUPPORTED_SELECTION:
		return V34_DATA_STREAM_OUT_OF_RANGE;
	case V34_FRAME_SCHEDULE_FRAME_COMPLETE:
		break;
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
	case V34_POWER_CALIBRATION_NUMERIC_FAILURE:
		return V34_DATA_STREAM_NUMERIC_FAILURE;
	}
	return V34_DATA_STREAM_NUMERIC_FAILURE;
}

struct b1_mapping_state {
	struct v34_frame_geometry geometry;
	struct v34_mapping_parameters parameters;
	struct v34_frame_schedule schedule;
	struct v34_scrambler scrambler;
	struct v34_4d_encoder encoder;
	struct v34_power_normalizer normalizer;
};

static enum v34_data_stream_result prepare_b1_mapping(
	const struct v34_data_stream_configuration *configuration,
	struct b1_mapping_state *state)
{
	struct b1_mapping_state prepared = { 0 };
	enum v34_data_stream_result result;

	if (configuration == NULL || state == NULL ||
	    !v34_phase3_profile_is_valid(&configuration->profile) ||
	    configuration->primary_data_rate_2400_bps_units >
		    configuration->profile
			    .projected_data_rate_2400_bps_units ||
	    configuration->sample_rate == 0 ||
	    configuration->nominal_peak_amplitude <= 0 ||
	    !v34_trellis_selection_is_valid(
		    configuration->trellis_selection) ||
	    !v34_nonlinear_parameter_is_valid(
		    configuration->nonlinear_parameter))
		return V34_DATA_STREAM_INVALID_ARGUMENT;
	result = geometry_result(v34_frame_geometry_init(
		&prepared.geometry, configuration->profile.symbol_rate,
		configuration->primary_data_rate_2400_bps_units,
		configuration->auxiliary_channel_enabled));
	if (result != V34_DATA_STREAM_OK)
		return result;
	if (v34_mapping_parameters_init(
		    &prepared.parameters, &prepared.geometry,
		    configuration->expanded_shaping) !=
	    V34_MAPPING_PARAMETERS_OK ||
	    v34_b1_frame_schedule_init(
		    &prepared.schedule, &prepared.geometry) !=
		    V34_FRAME_SCHEDULE_OK ||
	    v34_transmit_scrambler_init(
		    &prepared.scrambler,
		    configuration->profile.local_role) !=
		    V34_FRAMING_OK)
		return V34_DATA_STREAM_NUMERIC_FAILURE;
	if (v34_4d_encoder_init(
		    &prepared.encoder, &prepared.parameters,
		    configuration->precoder_coefficients,
		    configuration->trellis_selection) !=
	    V34_4D_ENCODER_OK)
		return V34_DATA_STREAM_INVALID_ARGUMENT;
	result = calibration_result(v34_calibrate_mapping_power(
		&prepared.schedule, &prepared.scrambler, &prepared.encoder,
		configuration->nonlinear_parameter, &prepared.normalizer));
	if (result != V34_DATA_STREAM_OK)
		return result;
	*state = prepared;
	return V34_DATA_STREAM_OK;
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
	struct b1_mapping_state mapping;
	struct v34_data_transmitter transmitter;
	enum v34_data_stream_result result;

	if (stream == NULL)
		return V34_DATA_STREAM_INVALID_ARGUMENT;
	result = prepare_b1_mapping(configuration, &mapping);
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
	struct b1_mapping_state mapping;
	struct v34_data_transmitter transmitter;
	enum v34_data_stream_result result;

	if (stream == NULL ||
	    !configuration_matches_line(configuration, active_line))
		return V34_DATA_STREAM_INVALID_ARGUMENT;
	result = prepare_b1_mapping(configuration, &mapping);
	if (result != V34_DATA_STREAM_OK)
		return result;
	result = transmitter_result(v34_data_transmitter_continue(
		&transmitter, active_line, &mapping.schedule,
		&mapping.scrambler, &mapping.encoder, &mapping.normalizer));
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

bool v34_data_stream_is_valid(
	const struct v34_data_stream *stream)
{
	return stream != NULL &&
	       v34_data_transmitter_is_valid(
		       &stream->transmitter) &&
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

	if (stream == NULL ||
	    !v34_data_transmitter_is_valid(transmitter) ||
	    !v34_data_transmitter_needs_mapping_frame(transmitter) ||
	    !schedule_is_at_b1_start(&transmitter->points.schedule))
		return V34_DATA_STREAM_INVALID_ARGUMENT;
	initialized = (struct v34_data_stream){
		.transmitter = *transmitter,
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

static bool bits_are_binary(const uint8_t *bits, size_t count)
{
	if (bits == NULL)
		return false;
	for (size_t index = 0; index < count; ++index) {
		if (bits[index] > 1u)
			return false;
	}
	return true;
}

static bool acquire_mapping_frame(
	const struct v34_data_stream *stream,
	v34_mapping_frame_source source, void *source_context,
	const struct v34_mapping_frame *request,
	uint8_t primary_bits[V34_MAX_MAPPING_FRAME_BITS],
	uint8_t *auxiliary_bit)
{
	uint8_t *auxiliary = request->carries_auxiliary_bit ?
		auxiliary_bit : NULL;

	if (stream->b1_active || source == NULL) {
		memset(primary_bits, 1, request->primary_bits);
		if (auxiliary != NULL)
			*auxiliary = 1;
		return true;
	}
	memset(primary_bits, 2, request->primary_bits);
	if (auxiliary != NULL)
		*auxiliary = 2;
	return source(
		       source_context, request, primary_bits, auxiliary) &&
	       bits_are_binary(primary_bits, request->primary_bits) &&
	       (auxiliary == NULL || *auxiliary <= 1u);
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
