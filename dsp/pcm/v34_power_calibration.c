// SPDX-License-Identifier: BSD-3-Clause

#include "v34_power_calibration.h"

#include "v34_data_points.h"
#include "v34_preemphasis.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>

static const uint32_t V34_POWER_CALIBRATION_SEED = UINT32_C(0x34c05eed);
static const double V34_LINE_POWER_EPSILON = 1e-12;

_Static_assert(V34_LINE_POWER_WARMUP_SUPERFRAMES <
		       V34_POWER_CALIBRATION_SUPERFRAMES,
	       "V.34 line calibration must retain a measurement interval");

static uint8_t next_generated_bit(uint32_t *state)
{
	*state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
	return (uint8_t)(*state >> 31);
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

static enum v34_power_calibration_result
mapping_transmitter_result(
	enum v34_mapping_frame_transmitter_result result)
{
	switch (result) {
	case V34_MAPPING_FRAME_TRANSMITTER_OK:
		return V34_POWER_CALIBRATION_OK;
	case V34_MAPPING_FRAME_TRANSMITTER_OUT_OF_RANGE:
		return V34_POWER_CALIBRATION_OUT_OF_RANGE;
	case V34_MAPPING_FRAME_TRANSMITTER_NUMERIC_FAILURE:
		return V34_POWER_CALIBRATION_NUMERIC_FAILURE;
	case V34_MAPPING_FRAME_TRANSMITTER_FRAME_COMPLETE:
	case V34_MAPPING_FRAME_TRANSMITTER_INVALID_ARGUMENT:
		break;
	}
	return V34_POWER_CALIBRATION_INVALID_ARGUMENT;
}

static enum v34_power_calibration_result symbol_power_result(
	enum v34_symbol_power_result result)
{
	switch (result) {
	case V34_SYMBOL_POWER_OK:
		return V34_POWER_CALIBRATION_OK;
	case V34_SYMBOL_POWER_OUT_OF_RANGE:
		return V34_POWER_CALIBRATION_OUT_OF_RANGE;
	case V34_SYMBOL_POWER_NUMERIC_FAILURE:
		return V34_POWER_CALIBRATION_NUMERIC_FAILURE;
	case V34_SYMBOL_POWER_INVALID_ARGUMENT:
		break;
	}
	return V34_POWER_CALIBRATION_INVALID_ARGUMENT;
}

static enum v34_power_calibration_result generate_mapping_frame(
	struct v34_frame_schedule *schedule,
	struct v34_scrambler *scrambler,
	struct v34_4d_encoder *encoder,
	enum v34_nonlinear_parameter nonlinear_parameter,
	double mean_squared_magnitude, uint32_t *random_state,
	struct v34_transmitted_mapping_frame *transmitted)
{
	struct v34_frame_schedule preview;
	struct v34_mapping_frame mapping_frame;
	uint8_t primary_bits[V34_MAX_MAPPING_FRAME_BITS];
	uint8_t auxiliary_bit;
	const uint8_t *auxiliary_pointer = NULL;
	enum v34_mapping_frame_transmitter_result result;

	if (v34_frame_schedule_is_complete(schedule) &&
	    v34_frame_schedule_advance_data_frame(schedule) !=
		    V34_FRAME_SCHEDULE_OK)
		return V34_POWER_CALIBRATION_INVALID_ARGUMENT;
	preview = *schedule;
	if (v34_frame_schedule_next(&preview, &mapping_frame) !=
	    V34_FRAME_SCHEDULE_OK)
		return V34_POWER_CALIBRATION_INVALID_ARGUMENT;
	for (size_t bit = 0; bit < mapping_frame.primary_bits; ++bit)
		primary_bits[bit] = next_generated_bit(random_state);
	if (mapping_frame.carries_auxiliary_bit) {
		auxiliary_bit = next_generated_bit(random_state);
		auxiliary_pointer = &auxiliary_bit;
	}
	result = v34_transmit_mapping_frame(
		schedule, scrambler, encoder, primary_bits,
		mapping_frame.primary_bits, auxiliary_pointer,
		nonlinear_parameter, mean_squared_magnitude, transmitted);
	return mapping_transmitter_result(result);
}

static enum v34_power_calibration_result measure_pass(
	const struct v34_frame_schedule *initial_schedule,
	const struct v34_scrambler *initial_scrambler,
	const struct v34_4d_encoder *initial_encoder,
	enum v34_nonlinear_parameter nonlinear_parameter,
	double mean_squared_magnitude, bool measure_projected,
	uint32_t mapping_frame_count,
	struct v34_symbol_power_measurement *measurement)
{
	struct v34_frame_schedule schedule = *initial_schedule;
	struct v34_scrambler scrambler = *initial_scrambler;
	struct v34_4d_encoder encoder = *initial_encoder;
	struct v34_symbol_power_measurement measured = { 0 };
	uint32_t random_state = V34_POWER_CALIBRATION_SEED;

	for (uint32_t frame = 0; frame < mapping_frame_count; ++frame) {
		struct v34_transmitted_mapping_frame transmitted;
		enum v34_power_calibration_result result =
			generate_mapping_frame(
				&schedule, &scrambler, &encoder,
				nonlinear_parameter, mean_squared_magnitude,
				&random_state, &transmitted);

		if (result != V34_POWER_CALIBRATION_OK)
			return result;
		for (size_t interval = 0;
		     interval < V34_4D_INTERVALS_PER_MAPPING_FRAME;
		     ++interval) {
			for (size_t symbol = 0;
			     symbol < V34_2D_SYMBOLS_PER_4D_INTERVAL;
			     ++symbol) {
				size_t index = interval *
					V34_2D_SYMBOLS_PER_4D_INTERVAL +
					symbol;
				enum v34_symbol_power_result
					measurement_result;

				if (measure_projected)
					measurement_result =
						v34_symbol_power_observe_point(
							&measured,
							&transmitted
								 .projected_symbol[index]);
				else
					measurement_result =
						v34_symbol_power_observe_q7(
							&measured,
							&transmitted.encoded
								 .interval[interval]
								 .precoded[symbol]
								 .precoded_symbol);
				result = symbol_power_result(measurement_result);
				if (result !=
				    V34_POWER_CALIBRATION_OK)
					return result;
			}
		}
	}
	*measurement = measured;
	return V34_POWER_CALIBRATION_OK;
}

enum v34_power_calibration_result
v34_calibrate_mapping_power(
	const struct v34_frame_schedule *initial_schedule,
	const struct v34_scrambler *initial_scrambler,
	const struct v34_4d_encoder *initial_encoder,
	enum v34_nonlinear_parameter nonlinear_parameter,
	struct v34_power_normalizer *normalizer)
{
	struct v34_symbol_power_measurement precoded;
	struct v34_symbol_power_measurement projected;
	struct v34_power_normalizer generated;
	uint32_t mapping_frame_count;
	enum v34_power_calibration_result result;

	if (!v34_frame_schedule_is_valid(initial_schedule) ||
	    !v34_scrambler_is_valid(initial_scrambler) ||
	    !v34_4d_encoder_is_valid(initial_encoder) ||
	    initial_schedule->geometry.high_mapping_frame_bits !=
		    initial_encoder->parameters.mapping_frame_bits ||
	    !v34_nonlinear_parameter_is_valid(
		    nonlinear_parameter) ||
	    normalizer == NULL)
		return V34_POWER_CALIBRATION_INVALID_ARGUMENT;
	mapping_frame_count =
		(uint32_t)V34_POWER_CALIBRATION_SUPERFRAMES *
		initial_schedule->geometry.data_frames_per_superframe *
		initial_schedule->geometry.mapping_frames_per_data_frame;
	result = measure_pass(
		initial_schedule, initial_scrambler, initial_encoder,
		V34_NONLINEAR_THETA_ZERO, 1.0, false,
		mapping_frame_count, &precoded);
	if (result != V34_POWER_CALIBRATION_OK)
		return result;
	result = measure_pass(
		initial_schedule, initial_scrambler, initial_encoder,
		nonlinear_parameter, precoded.mean_squared_magnitude, true,
		mapping_frame_count, &projected);
	if (result != V34_POWER_CALIBRATION_OK)
		return result;
	result = symbol_power_result(v34_power_normalizer_init(
		&generated, &initial_encoder->parameters, nonlinear_parameter,
		&precoded, &projected));
	if (result == V34_POWER_CALIBRATION_OK)
		*normalizer = generated;
	return result;
}

bool v34_line_power_measurement_is_valid(
	const struct v34_line_power_measurement *measurement)
{
	return measurement != NULL && measurement->sample_count != 0 &&
	       isfinite(measurement->mean_squared_amplitude) &&
	       measurement->mean_squared_amplitude > 0.0 &&
	       isfinite(measurement->peak_amplitude) &&
	       measurement->peak_amplitude > 0.0 &&
	       sqrt(measurement->mean_squared_amplitude) <=
		       measurement->peak_amplitude *
			       (1.0 + V34_LINE_POWER_EPSILON);
}

static enum v34_power_calibration_result observe_line_sample(
	struct v34_line_power_measurement *measurement,
	double sample)
{
	double squared;
	double magnitude;

	if (!isfinite(sample))
		return V34_POWER_CALIBRATION_NUMERIC_FAILURE;
	if (measurement->sample_count == UINT32_MAX)
		return V34_POWER_CALIBRATION_OUT_OF_RANGE;
	squared = sample * sample;
	magnitude = fabs(sample);
	if (!isfinite(squared))
		return V34_POWER_CALIBRATION_NUMERIC_FAILURE;
	measurement->sample_count++;
	measurement->mean_squared_amplitude +=
		(squared - measurement->mean_squared_amplitude) /
		measurement->sample_count;
	if (magnitude > measurement->peak_amplitude)
		measurement->peak_amplitude = magnitude;
	return isfinite(measurement->mean_squared_amplitude) ?
		       V34_POWER_CALIBRATION_OK :
		       V34_POWER_CALIBRATION_NUMERIC_FAILURE;
}

static enum v34_power_calibration_result carrier_result(
	enum v34_data_carrier_result result)
{
	switch (result) {
	case V34_DATA_CARRIER_OK:
		return V34_POWER_CALIBRATION_OK;
	case V34_DATA_CARRIER_OUT_OF_RANGE:
		return V34_POWER_CALIBRATION_OUT_OF_RANGE;
	case V34_DATA_CARRIER_NUMERIC_FAILURE:
		return V34_POWER_CALIBRATION_NUMERIC_FAILURE;
	case V34_DATA_CARRIER_INVALID_ARGUMENT:
		break;
	}
	return V34_POWER_CALIBRATION_INVALID_ARGUMENT;
}

static enum v34_power_calibration_result data_points_result(
	enum v34_data_points_result result)
{
	switch (result) {
	case V34_DATA_POINTS_OK:
		return V34_POWER_CALIBRATION_OK;
	case V34_DATA_POINTS_OUT_OF_RANGE:
		return V34_POWER_CALIBRATION_OUT_OF_RANGE;
	case V34_DATA_POINTS_NUMERIC_FAILURE:
	case V34_DATA_POINTS_MAPPING_FRAME_REQUIRED:
	case V34_DATA_POINTS_MAPPING_FRAME_PENDING:
		return V34_POWER_CALIBRATION_NUMERIC_FAILURE;
	case V34_DATA_POINTS_INVALID_ARGUMENT:
		break;
	}
	return V34_POWER_CALIBRATION_INVALID_ARGUMENT;
}

static enum v34_power_calibration_result queue_generated_points(
	struct v34_data_points *points, uint32_t *random_state)
{
	struct v34_mapping_frame request;
	uint8_t primary_bits[V34_MAX_MAPPING_FRAME_BITS];
	uint8_t auxiliary_bit;
	const uint8_t *auxiliary_pointer = NULL;
	enum v34_power_calibration_result result;

	result = data_points_result(v34_data_points_preview_mapping_frame(
		points, &request));
	if (result != V34_POWER_CALIBRATION_OK)
		return result;
	for (size_t bit = 0; bit < request.primary_bits; ++bit)
		primary_bits[bit] = next_generated_bit(random_state);
	if (request.carries_auxiliary_bit) {
		auxiliary_bit = next_generated_bit(random_state);
		auxiliary_pointer = &auxiliary_bit;
	}
	return data_points_result(v34_data_points_queue_mapping_frame(
		points, primary_bits, request.primary_bits, auxiliary_pointer));
}

enum v34_power_calibration_result
v34_calibrate_line_power(
	const struct v34_frame_schedule *initial_schedule,
	const struct v34_scrambler *initial_scrambler,
	const struct v34_4d_encoder *initial_encoder,
	const struct v34_power_normalizer *normalizer,
	const struct v34_phase3_profile *profile,
	uint32_t sample_rate,
	struct v34_line_power_measurement *measurement)
{
	struct v34_data_points points;
	struct v34_data_carrier carrier;
	struct v34_preemphasis preemphasis;
	struct v34_line_power_measurement measured = { 0 };
	uint32_t random_state = V34_POWER_CALIBRATION_SEED;
	uint32_t frames_per_superframe;
	uint32_t frame_count;
	uint32_t warmup_frames;
	uint64_t maximum_frame_samples;
	enum v34_preemphasis_result preemphasis_result;

	if (!v34_phase3_profile_is_valid(profile) ||
	    !v34_frame_schedule_is_valid(initial_schedule) ||
	    !v34_transmit_scrambler_is_valid(
		    initial_scrambler, profile->local_role) ||
	    !v34_4d_encoder_is_valid(initial_encoder) ||
	    !v34_power_normalizer_is_valid(normalizer) ||
	    !mapping_parameters_are_equal(
		    &initial_encoder->parameters, &normalizer->parameters) ||
	    measurement == NULL)
		return V34_POWER_CALIBRATION_INVALID_ARGUMENT;
	if (v34_data_points_init(
		    &points, initial_schedule, initial_scrambler,
		    initial_encoder, normalizer, profile->local_role) !=
	    V34_DATA_POINTS_OK)
		return V34_POWER_CALIBRATION_INVALID_ARGUMENT;
	if (v34_data_carrier_init(
		    &carrier, profile, &points.schedule.geometry, normalizer,
		    sample_rate) != V34_DATA_CARRIER_OK)
		return V34_POWER_CALIBRATION_INVALID_ARGUMENT;
	preemphasis_result = v34_preemphasis_init(
		&preemphasis, sample_rate, profile->symbol_rate_numerator,
		profile->symbol_rate_denominator,
		profile->preemphasis_filter_index);
	if (preemphasis_result == V34_PREEMPHASIS_NUMERIC_FAILURE)
		return V34_POWER_CALIBRATION_NUMERIC_FAILURE;
	if (preemphasis_result != V34_PREEMPHASIS_OK)
		return V34_POWER_CALIBRATION_INVALID_ARGUMENT;
	frames_per_superframe =
		(uint32_t)points.schedule.geometry.data_frames_per_superframe *
		points.schedule.geometry.mapping_frames_per_data_frame;
	frame_count = V34_POWER_CALIBRATION_SUPERFRAMES *
		frames_per_superframe;
	warmup_frames = V34_LINE_POWER_WARMUP_SUPERFRAMES *
		frames_per_superframe;
	maximum_frame_samples =
		(uint64_t)V34_SYMBOLS_PER_MAPPING_FRAME *
		(((uint64_t)sample_rate * profile->symbol_rate_denominator +
		  profile->symbol_rate_numerator - 1u) /
			 profile->symbol_rate_numerator +
		 1u);
	for (uint32_t frame = 0; frame < frame_count; ++frame) {
		enum v34_power_calibration_result result;
		uint64_t frame_samples = 0;

		result = queue_generated_points(&points, &random_state);
		if (result != V34_POWER_CALIBRATION_OK)
			return result;
		while (!v34_data_points_needs_mapping_frame(&points)) {
			struct qam_point point;
			const struct qam_point *point_pointer = NULL;
			double carrier_sample;
			double line_sample;
			bool symbol_accepted;

			if (++frame_samples > maximum_frame_samples)
				return V34_POWER_CALIBRATION_NUMERIC_FAILURE;
			if (v34_data_carrier_needs_point(&carrier)) {
				result = data_points_result(v34_data_points_next(
					&points, &point));
				if (result != V34_POWER_CALIBRATION_OK)
					return result;
				point_pointer = &point;
			}
			result = carrier_result(v34_data_carrier_sample(
				&carrier, point_pointer, &carrier_sample,
				&symbol_accepted));
			if (result != V34_POWER_CALIBRATION_OK)
				return result;
			if (symbol_accepted != (point_pointer != NULL))
				return V34_POWER_CALIBRATION_NUMERIC_FAILURE;
			if (v34_preemphasis_sample(
				    &preemphasis, carrier_sample, &line_sample) !=
			    V34_PREEMPHASIS_OK)
				return V34_POWER_CALIBRATION_NUMERIC_FAILURE;
			if (frame >= warmup_frames) {
				result = observe_line_sample(&measured, line_sample);
				if (result !=
				    V34_POWER_CALIBRATION_OK)
					return result;
			}
		}
	}
	if (!v34_line_power_measurement_is_valid(&measured))
		return V34_POWER_CALIBRATION_NUMERIC_FAILURE;
	*measurement = measured;
	return V34_POWER_CALIBRATION_OK;
}

const char *v34_power_calibration_result_name(
	enum v34_power_calibration_result result)
{
	switch (result) {
	case V34_POWER_CALIBRATION_OK:
		return "ok";
	case V34_POWER_CALIBRATION_INVALID_ARGUMENT:
		return "invalid argument";
	case V34_POWER_CALIBRATION_OUT_OF_RANGE:
		return "out of range";
	case V34_POWER_CALIBRATION_NUMERIC_FAILURE:
		return "numeric failure";
	}
	return "unknown";
}
