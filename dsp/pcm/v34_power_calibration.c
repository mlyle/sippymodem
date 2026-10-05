// SPDX-License-Identifier: BSD-3-Clause

#include "v34_power_calibration.h"

#include "v34_data_points.h"
#include "v34_pulse_modulator.h"
#include "v34_rate_profile.h"
#include "v34_training_line.h"

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
	struct v34_transmitted_mapping_frame *transmitted, bool project)
{
	struct v34_frame_schedule preview;
	struct v34_mapping_frame mapping_frame;
	uint8_t primary_bits[V34_MAX_MAPPING_FRAME_BITS];
	uint8_t auxiliary_bit;
	const uint8_t *auxiliary_pointer = NULL;
	enum v34_mapping_frame_transmitter_result result;

	if (v34_frame_schedule_prepare_mapping_frame(schedule) !=
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
	result = project ? v34_transmit_mapping_frame(
		schedule, scrambler, encoder, primary_bits,
		mapping_frame.primary_bits, auxiliary_pointer,
		nonlinear_parameter, mean_squared_magnitude, transmitted) :
		v34_transmit_mapping_frame_q7(schedule, scrambler, encoder, primary_bits,
			mapping_frame.primary_bits, auxiliary_pointer, transmitted);
	return mapping_transmitter_result(result);
}

enum v34_power_calibration_result v34_mapping_power_calibration_begin(
	struct v34_mapping_power_calibration *calibration,
	const struct v34_frame_schedule *initial_schedule,
	const struct v34_scrambler *initial_scrambler,
	const struct v34_4d_encoder *initial_encoder,
	enum v34_nonlinear_parameter nonlinear_parameter)
{
	if (calibration == NULL ||
	    !v34_frame_schedule_is_valid(initial_schedule) ||
	    !v34_scrambler_is_valid(initial_scrambler) ||
	    !v34_4d_encoder_is_valid(initial_encoder) ||
	    initial_schedule->geometry.high_mapping_frame_bits !=
		    initial_encoder->parameters.mapping_frame_bits ||
	    !v34_nonlinear_parameter_is_valid(nonlinear_parameter))
		return V34_POWER_CALIBRATION_INVALID_ARGUMENT;
	*calibration = (struct v34_mapping_power_calibration){
		.initial_schedule = *initial_schedule, .schedule = *initial_schedule,
		.initial_scrambler = *initial_scrambler, .scrambler = *initial_scrambler,
		.initial_encoder = *initial_encoder, .encoder = *initial_encoder,
		.random_state = V34_POWER_CALIBRATION_SEED,
		.frame_count = (uint32_t)V34_POWER_CALIBRATION_SUPERFRAMES *
			initial_schedule->geometry.data_frames_per_superframe *
			initial_schedule->geometry.mapping_frames_per_data_frame,
		.nonlinear_parameter = nonlinear_parameter,
	};
	return V34_POWER_CALIBRATION_OK;
}

static enum v34_power_calibration_result observe_mapping_frame(
	struct v34_mapping_power_calibration *calibration)
{
	struct v34_transmitted_mapping_frame transmitted;
	struct v34_symbol_power_measurement *measured =
		calibration->measuring_projected ? &calibration->projected :
			&calibration->precoded;
	enum v34_power_calibration_result result = generate_mapping_frame(
		&calibration->schedule, &calibration->scrambler, &calibration->encoder,
		calibration->measuring_projected ? calibration->nonlinear_parameter :
			V34_NONLINEAR_THETA_ZERO,
		calibration->measuring_projected ?
			calibration->precoded.mean_squared_magnitude : 1.0,
		&calibration->random_state, &transmitted, calibration->measuring_projected);

	if (result != V34_POWER_CALIBRATION_OK)
		return result;
	for (size_t interval = 0; interval < V34_4D_INTERVALS_PER_MAPPING_FRAME;
	     ++interval) {
		for (size_t symbol = 0; symbol < V34_2D_SYMBOLS_PER_4D_INTERVAL;
		     ++symbol) {
			size_t index = interval * V34_2D_SYMBOLS_PER_4D_INTERVAL + symbol;
			result = symbol_power_result(calibration->measuring_projected ?
				v34_symbol_power_observe_point(measured,
					&transmitted.projected_symbol[index]) :
				v34_symbol_power_observe_q7(measured,
					&transmitted.encoded.interval[interval]
						.precoded[symbol].precoded_symbol));
			if (result != V34_POWER_CALIBRATION_OK)
				return result;
		}
	}
	return V34_POWER_CALIBRATION_OK;
}

static enum v34_power_calibration_result finish_mapping_power(
	const struct v34_mapping_power_calibration *calibration,
	struct v34_power_normalizer *normalizer)
{
	struct v34_symbol_power_measurement precoded = calibration->precoded;
	struct v34_symbol_power_measurement projected = calibration->measuring_projected ?
		calibration->projected : precoded;
	struct v34_power_normalizer generated;
	enum v34_nonlinear_parameter nonlinear_parameter = calibration->nonlinear_parameter;
	uint16_t rate_level;
	double normalization_gain;
	enum v34_power_calibration_result result;

	result = symbol_power_result(v34_power_normalizer_init(
		&generated, &calibration->initial_encoder.parameters, nonlinear_parameter,
		&precoded, &projected));
	if (result != V34_POWER_CALIBRATION_OK)
		return result;
	if (v34_rate_profile_level(
		    calibration->initial_schedule.geometry.symbol_rate,
		    calibration->initial_schedule.geometry.primary_data_rate_2400_bps_units,
		    calibration->initial_encoder.parameters.expanded_shaping, &rate_level) !=
	    V34_RATE_PROFILE_OK)
		return V34_POWER_CALIBRATION_OUT_OF_RANGE;
	normalization_gain = (double)rate_level *
		V34_TRAINING_PULSE_POINT_ZERO_Q12 /
		((double)V34_PULSE_POINT_SCALE *
		 V34_TRAINING_FIXED_POINT_ZERO);
	/* The rate profile sets the linear mean-power scale.  Preserve that
	 * mean after the radial nonlinear projection instead of transmitting
	 * the projection's additional power. */
	normalization_gain *= sqrt(
		precoded.mean_squared_magnitude /
		projected.mean_squared_magnitude);
	result = symbol_power_result(v34_power_normalizer_set_gain(
		&generated, normalization_gain));
	if (result == V34_POWER_CALIBRATION_OK)
		*normalizer = generated;
	return result;
}
enum v34_power_calibration_result v34_mapping_power_calibration_step(
	struct v34_mapping_power_calibration *calibration,
	uint32_t frame_budget, struct v34_power_normalizer *normalizer)
{
	if (calibration == NULL || normalizer == NULL || frame_budget == 0 ||
	    calibration->frame_count == 0 || calibration->frame > calibration->frame_count)
		return V34_POWER_CALIBRATION_INVALID_ARGUMENT;
	while (frame_budget != 0) {
		if (calibration->frame == calibration->frame_count) {
			if (calibration->measuring_projected ||
			    calibration->nonlinear_parameter == V34_NONLINEAR_THETA_ZERO)
				return finish_mapping_power(calibration, normalizer);
			calibration->schedule = calibration->initial_schedule;
			calibration->scrambler = calibration->initial_scrambler;
			calibration->encoder = calibration->initial_encoder;
			calibration->random_state = V34_POWER_CALIBRATION_SEED;
			calibration->frame = 0;
			calibration->measuring_projected = true;
		}
		enum v34_power_calibration_result result = observe_mapping_frame(calibration);
		if (result != V34_POWER_CALIBRATION_OK)
			return result;
		calibration->frame++;
		frame_budget--;
	}
	if (calibration->frame == calibration->frame_count &&
	    (calibration->measuring_projected ||
	     calibration->nonlinear_parameter == V34_NONLINEAR_THETA_ZERO))
		return finish_mapping_power(calibration, normalizer);
	return V34_POWER_CALIBRATION_IN_PROGRESS;
}

enum v34_power_calibration_result v34_calibrate_mapping_power(
	const struct v34_frame_schedule *initial_schedule,
	const struct v34_scrambler *initial_scrambler,
	const struct v34_4d_encoder *initial_encoder,
	enum v34_nonlinear_parameter nonlinear_parameter,
	struct v34_power_normalizer *normalizer)
{
	struct v34_mapping_power_calibration calibration;
	enum v34_power_calibration_result result;

	if (normalizer == NULL)
		return V34_POWER_CALIBRATION_INVALID_ARGUMENT;
	result = v34_mapping_power_calibration_begin(&calibration, initial_schedule,
		initial_scrambler, initial_encoder, nonlinear_parameter);
	return result == V34_POWER_CALIBRATION_OK ?
		v34_mapping_power_calibration_step(&calibration, UINT32_MAX, normalizer) : result;
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

static enum v34_power_calibration_result pulse_result(
	enum v34_pulse_modulator_result result)
{
	switch (result) {
	case V34_PULSE_MODULATOR_OK:
		return V34_POWER_CALIBRATION_OK;
	case V34_PULSE_MODULATOR_NUMERIC_FAILURE:
		return V34_POWER_CALIBRATION_NUMERIC_FAILURE;
	case V34_PULSE_MODULATOR_INVALID_ARGUMENT:
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
v34_line_power_calibration_begin(
	struct v34_line_power_calibration *calibration,
	const struct v34_frame_schedule *initial_schedule,
	const struct v34_scrambler *initial_scrambler,
	const struct v34_4d_encoder *initial_encoder,
	const struct v34_power_normalizer *normalizer,
	const struct v34_phase3_profile *profile,
	uint32_t sample_rate)
{
	struct v34_line_power_calibration prepared = { 0 };
	struct qam_channel channel;
	uint32_t frames_per_superframe;
	uint32_t frame_count;
	uint32_t warmup_frames;
	uint64_t maximum_frame_samples;

	if (!v34_phase3_profile_is_valid(profile) ||
	    !v34_frame_schedule_is_valid(initial_schedule) ||
	    initial_schedule->geometry.symbol_rate != profile->symbol_rate ||
	    !v34_transmit_scrambler_is_valid(
		    initial_scrambler, profile->local_role) ||
	    !v34_4d_encoder_is_valid(initial_encoder) ||
	    !v34_power_normalizer_is_valid(normalizer) ||
	    !v34_mapping_parameters_are_equal(
		    &initial_encoder->parameters, &normalizer->parameters) ||
	    calibration == NULL)
		return V34_POWER_CALIBRATION_INVALID_ARGUMENT;
	if (v34_data_points_init(
		    &prepared.points, initial_schedule, initial_scrambler,
		    initial_encoder, normalizer, profile->local_role) !=
	    V34_DATA_POINTS_OK)
		return V34_POWER_CALIBRATION_INVALID_ARGUMENT;
	if (v34_line_channel_init(profile, sample_rate, &channel) !=
		    V34_LINE_CHANNEL_OK ||
	    v34_pulse_modulator_init(
		    &prepared.carrier, profile, &channel,
		    normalizer->maximum_normalized_point_power) !=
		    V34_PULSE_MODULATOR_OK)
		return V34_POWER_CALIBRATION_INVALID_ARGUMENT;
	frames_per_superframe =
		(uint32_t)prepared.points.schedule.geometry.data_frames_per_superframe *
		prepared.points.schedule.geometry.mapping_frames_per_data_frame;
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
	prepared.random_state = V34_POWER_CALIBRATION_SEED;
	prepared.frame_count = frame_count;
	prepared.warmup_frames = warmup_frames;
	prepared.maximum_frame_samples = maximum_frame_samples;
	*calibration = prepared;
	return V34_POWER_CALIBRATION_OK;
}

enum v34_power_calibration_result v34_line_power_calibration_step(
	struct v34_line_power_calibration *calibration,
	uint32_t frame_budget, struct v34_line_power_measurement *measurement)
{
	if (calibration == NULL || measurement == NULL || frame_budget == 0 ||
	    calibration->frame_count == 0 || calibration->frame > calibration->frame_count ||
	    !v34_pulse_modulator_is_valid(&calibration->carrier) ||
	    !v34_data_points_is_valid(&calibration->points))
		return V34_POWER_CALIBRATION_INVALID_ARGUMENT;
	for (; calibration->frame < calibration->frame_count && frame_budget != 0;
	     ++calibration->frame, --frame_budget) {
		enum v34_power_calibration_result result;
		uint64_t frame_samples = 0;

		result = queue_generated_points(&calibration->points, &calibration->random_state);
		if (result != V34_POWER_CALIBRATION_OK)
			return result;
		/* queue_generated_points validates every normalized point. Only the
		 * owned cursor changes while draining this frame into the waveform. */
		while (calibration->points.next_point < V34_SYMBOLS_PER_MAPPING_FRAME) {
			struct qam_point point;
			const struct qam_point *point_pointer = NULL;
			double line_sample;
			bool symbol_accepted;

			if (++frame_samples > calibration->maximum_frame_samples)
				return V34_POWER_CALIBRATION_NUMERIC_FAILURE;
			if (calibration->carrier.symbol_phase_units <
			    calibration->carrier.channel.symbol_rate_numerator) {
				point = calibration->points.point[calibration->points.next_point++];
				point_pointer = &point;
			}
			result = pulse_result(v34_pulse_modulator_sample_owned(
				&calibration->carrier, point_pointer, &line_sample,
				&symbol_accepted));
			if (result != V34_POWER_CALIBRATION_OK)
				return result;
			if (symbol_accepted != (point_pointer != NULL))
				return V34_POWER_CALIBRATION_NUMERIC_FAILURE;
			if (calibration->frame >= calibration->warmup_frames) {
				result = observe_line_sample(&calibration->measured, line_sample);
				if (result !=
				    V34_POWER_CALIBRATION_OK)
					return result;
			}
		}
	}
	if (calibration->frame != calibration->frame_count)
		return V34_POWER_CALIBRATION_IN_PROGRESS;
	if (!v34_line_power_measurement_is_valid(&calibration->measured))
		return V34_POWER_CALIBRATION_NUMERIC_FAILURE;
	*measurement = calibration->measured;
	return V34_POWER_CALIBRATION_OK;
}

enum v34_power_calibration_result v34_calibrate_line_power(
	const struct v34_frame_schedule *initial_schedule,
	const struct v34_scrambler *initial_scrambler,
	const struct v34_4d_encoder *initial_encoder,
	const struct v34_power_normalizer *normalizer,
	const struct v34_phase3_profile *profile,
	uint32_t sample_rate, struct v34_line_power_measurement *measurement)
{
	struct v34_line_power_calibration calibration;
	enum v34_power_calibration_result result;

	if (measurement == NULL)
		return V34_POWER_CALIBRATION_INVALID_ARGUMENT;
	result = v34_line_power_calibration_begin(&calibration, initial_schedule,
		initial_scrambler, initial_encoder, normalizer, profile, sample_rate);
	return result == V34_POWER_CALIBRATION_OK ?
		v34_line_power_calibration_step(&calibration, UINT32_MAX, measurement) : result;
}
