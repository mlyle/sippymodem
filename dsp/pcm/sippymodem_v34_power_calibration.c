// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_v34_power_calibration.h"

#include "sippymodem_v34_preemphasis.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>

static const uint32_t V34_POWER_CALIBRATION_SEED = UINT32_C(0x34c05eed);
static const double V34_LINE_POWER_EPSILON = 1e-12;

_Static_assert(SIPPYMODEM_V34_LINE_POWER_WARMUP_SUPERFRAMES <
		       SIPPYMODEM_V34_POWER_CALIBRATION_SUPERFRAMES,
	       "V.34 line calibration must retain a measurement interval");

static uint8_t next_generated_bit(uint32_t *state)
{
	*state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
	return (uint8_t)(*state >> 31);
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

static enum sippymodem_v34_power_calibration_result
mapping_transmitter_result(
	enum sippymodem_v34_mapping_frame_transmitter_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_MAPPING_FRAME_TRANSMITTER_OK:
		return SIPPYMODEM_V34_POWER_CALIBRATION_OK;
	case SIPPYMODEM_V34_MAPPING_FRAME_TRANSMITTER_OUT_OF_RANGE:
		return SIPPYMODEM_V34_POWER_CALIBRATION_OUT_OF_RANGE;
	case SIPPYMODEM_V34_MAPPING_FRAME_TRANSMITTER_NUMERIC_FAILURE:
		return SIPPYMODEM_V34_POWER_CALIBRATION_NUMERIC_FAILURE;
	case SIPPYMODEM_V34_MAPPING_FRAME_TRANSMITTER_FRAME_COMPLETE:
	case SIPPYMODEM_V34_MAPPING_FRAME_TRANSMITTER_INVALID_ARGUMENT:
		break;
	}
	return SIPPYMODEM_V34_POWER_CALIBRATION_INVALID_ARGUMENT;
}

static enum sippymodem_v34_power_calibration_result symbol_power_result(
	enum sippymodem_v34_symbol_power_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_SYMBOL_POWER_OK:
		return SIPPYMODEM_V34_POWER_CALIBRATION_OK;
	case SIPPYMODEM_V34_SYMBOL_POWER_OUT_OF_RANGE:
		return SIPPYMODEM_V34_POWER_CALIBRATION_OUT_OF_RANGE;
	case SIPPYMODEM_V34_SYMBOL_POWER_NUMERIC_FAILURE:
		return SIPPYMODEM_V34_POWER_CALIBRATION_NUMERIC_FAILURE;
	case SIPPYMODEM_V34_SYMBOL_POWER_INVALID_ARGUMENT:
		break;
	}
	return SIPPYMODEM_V34_POWER_CALIBRATION_INVALID_ARGUMENT;
}

static enum sippymodem_v34_power_calibration_result generate_mapping_frame(
	struct sippymodem_v34_frame_schedule *schedule,
	struct sippymodem_v34_scrambler *scrambler,
	struct sippymodem_v34_4d_encoder *encoder,
	enum sippymodem_v34_nonlinear_parameter nonlinear_parameter,
	double mean_squared_magnitude, uint32_t *random_state,
	struct sippymodem_v34_transmitted_mapping_frame *transmitted)
{
	struct sippymodem_v34_frame_schedule preview;
	struct sippymodem_v34_mapping_frame mapping_frame;
	uint8_t primary_bits[SIPPYMODEM_V34_MAX_MAPPING_FRAME_BITS];
	uint8_t auxiliary_bit;
	const uint8_t *auxiliary_pointer = NULL;
	enum sippymodem_v34_mapping_frame_transmitter_result result;

	if (sippymodem_v34_frame_schedule_is_complete(schedule) &&
	    sippymodem_v34_frame_schedule_advance_data_frame(schedule) !=
		    SIPPYMODEM_V34_FRAME_SCHEDULE_OK)
		return SIPPYMODEM_V34_POWER_CALIBRATION_INVALID_ARGUMENT;
	preview = *schedule;
	if (sippymodem_v34_frame_schedule_next(&preview, &mapping_frame) !=
	    SIPPYMODEM_V34_FRAME_SCHEDULE_OK)
		return SIPPYMODEM_V34_POWER_CALIBRATION_INVALID_ARGUMENT;
	for (size_t bit = 0; bit < mapping_frame.primary_bits; ++bit)
		primary_bits[bit] = next_generated_bit(random_state);
	if (mapping_frame.carries_auxiliary_bit) {
		auxiliary_bit = next_generated_bit(random_state);
		auxiliary_pointer = &auxiliary_bit;
	}
	result = sippymodem_v34_transmit_mapping_frame(
		schedule, scrambler, encoder, primary_bits,
		mapping_frame.primary_bits, auxiliary_pointer,
		nonlinear_parameter, mean_squared_magnitude, transmitted);
	return mapping_transmitter_result(result);
}

static enum sippymodem_v34_power_calibration_result measure_pass(
	const struct sippymodem_v34_frame_schedule *initial_schedule,
	const struct sippymodem_v34_scrambler *initial_scrambler,
	const struct sippymodem_v34_4d_encoder *initial_encoder,
	enum sippymodem_v34_nonlinear_parameter nonlinear_parameter,
	double mean_squared_magnitude, bool measure_projected,
	uint32_t mapping_frame_count,
	struct sippymodem_v34_symbol_power_measurement *measurement)
{
	struct sippymodem_v34_frame_schedule schedule = *initial_schedule;
	struct sippymodem_v34_scrambler scrambler = *initial_scrambler;
	struct sippymodem_v34_4d_encoder encoder = *initial_encoder;
	struct sippymodem_v34_symbol_power_measurement measured = { 0 };
	uint32_t random_state = V34_POWER_CALIBRATION_SEED;

	for (uint32_t frame = 0; frame < mapping_frame_count; ++frame) {
		struct sippymodem_v34_transmitted_mapping_frame transmitted;
		enum sippymodem_v34_power_calibration_result result =
			generate_mapping_frame(
				&schedule, &scrambler, &encoder,
				nonlinear_parameter, mean_squared_magnitude,
				&random_state, &transmitted);

		if (result != SIPPYMODEM_V34_POWER_CALIBRATION_OK)
			return result;
		for (size_t interval = 0;
		     interval < SIPPYMODEM_V34_4D_INTERVALS_PER_MAPPING_FRAME;
		     ++interval) {
			for (size_t symbol = 0;
			     symbol < SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL;
			     ++symbol) {
				size_t index = interval *
					SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL +
					symbol;
				enum sippymodem_v34_symbol_power_result
					measurement_result;

				if (measure_projected)
					measurement_result =
						sippymodem_v34_symbol_power_observe_point(
							&measured,
							&transmitted
								 .projected_symbol[index]);
				else
					measurement_result =
						sippymodem_v34_symbol_power_observe_q7(
							&measured,
							&transmitted.encoded
								 .interval[interval]
								 .precoded[symbol]
								 .precoded_symbol);
				result = symbol_power_result(measurement_result);
				if (result !=
				    SIPPYMODEM_V34_POWER_CALIBRATION_OK)
					return result;
			}
		}
	}
	*measurement = measured;
	return SIPPYMODEM_V34_POWER_CALIBRATION_OK;
}

enum sippymodem_v34_power_calibration_result
sippymodem_v34_calibrate_mapping_power(
	const struct sippymodem_v34_frame_schedule *initial_schedule,
	const struct sippymodem_v34_scrambler *initial_scrambler,
	const struct sippymodem_v34_4d_encoder *initial_encoder,
	enum sippymodem_v34_nonlinear_parameter nonlinear_parameter,
	struct sippymodem_v34_power_normalizer *normalizer)
{
	struct sippymodem_v34_symbol_power_measurement precoded;
	struct sippymodem_v34_symbol_power_measurement projected;
	struct sippymodem_v34_power_normalizer generated;
	uint32_t mapping_frame_count;
	enum sippymodem_v34_power_calibration_result result;

	if (!sippymodem_v34_frame_schedule_is_valid(initial_schedule) ||
	    !sippymodem_v34_scrambler_is_valid(initial_scrambler) ||
	    !sippymodem_v34_4d_encoder_is_valid(initial_encoder) ||
	    initial_schedule->geometry.high_mapping_frame_bits !=
		    initial_encoder->parameters.mapping_frame_bits ||
	    !sippymodem_v34_nonlinear_parameter_is_valid(
		    nonlinear_parameter) ||
	    normalizer == NULL)
		return SIPPYMODEM_V34_POWER_CALIBRATION_INVALID_ARGUMENT;
	mapping_frame_count =
		(uint32_t)SIPPYMODEM_V34_POWER_CALIBRATION_SUPERFRAMES *
		initial_schedule->geometry.data_frames_per_superframe *
		initial_schedule->geometry.mapping_frames_per_data_frame;
	result = measure_pass(
		initial_schedule, initial_scrambler, initial_encoder,
		SIPPYMODEM_V34_NONLINEAR_THETA_ZERO, 1.0, false,
		mapping_frame_count, &precoded);
	if (result != SIPPYMODEM_V34_POWER_CALIBRATION_OK)
		return result;
	result = measure_pass(
		initial_schedule, initial_scrambler, initial_encoder,
		nonlinear_parameter, precoded.mean_squared_magnitude, true,
		mapping_frame_count, &projected);
	if (result != SIPPYMODEM_V34_POWER_CALIBRATION_OK)
		return result;
	result = symbol_power_result(sippymodem_v34_power_normalizer_init(
		&generated, &initial_encoder->parameters, nonlinear_parameter,
		&precoded, &projected));
	if (result == SIPPYMODEM_V34_POWER_CALIBRATION_OK)
		*normalizer = generated;
	return result;
}

bool sippymodem_v34_line_power_measurement_is_valid(
	const struct sippymodem_v34_line_power_measurement *measurement)
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

static enum sippymodem_v34_power_calibration_result observe_line_sample(
	struct sippymodem_v34_line_power_measurement *measurement,
	double sample)
{
	double squared;
	double magnitude;

	if (!isfinite(sample))
		return SIPPYMODEM_V34_POWER_CALIBRATION_NUMERIC_FAILURE;
	if (measurement->sample_count == UINT32_MAX)
		return SIPPYMODEM_V34_POWER_CALIBRATION_OUT_OF_RANGE;
	squared = sample * sample;
	magnitude = fabs(sample);
	if (!isfinite(squared))
		return SIPPYMODEM_V34_POWER_CALIBRATION_NUMERIC_FAILURE;
	measurement->sample_count++;
	measurement->mean_squared_amplitude +=
		(squared - measurement->mean_squared_amplitude) /
		measurement->sample_count;
	if (magnitude > measurement->peak_amplitude)
		measurement->peak_amplitude = magnitude;
	return isfinite(measurement->mean_squared_amplitude) ?
		       SIPPYMODEM_V34_POWER_CALIBRATION_OK :
		       SIPPYMODEM_V34_POWER_CALIBRATION_NUMERIC_FAILURE;
}

static enum sippymodem_v34_power_calibration_result carrier_result(
	enum sippymodem_v34_data_carrier_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_DATA_CARRIER_OK:
		return SIPPYMODEM_V34_POWER_CALIBRATION_OK;
	case SIPPYMODEM_V34_DATA_CARRIER_OUT_OF_RANGE:
		return SIPPYMODEM_V34_POWER_CALIBRATION_OUT_OF_RANGE;
	case SIPPYMODEM_V34_DATA_CARRIER_NUMERIC_FAILURE:
		return SIPPYMODEM_V34_POWER_CALIBRATION_NUMERIC_FAILURE;
	case SIPPYMODEM_V34_DATA_CARRIER_MAPPING_FRAME_REQUIRED:
	case SIPPYMODEM_V34_DATA_CARRIER_MAPPING_FRAME_PENDING:
		return SIPPYMODEM_V34_POWER_CALIBRATION_NUMERIC_FAILURE;
	case SIPPYMODEM_V34_DATA_CARRIER_INVALID_ARGUMENT:
		break;
	}
	return SIPPYMODEM_V34_POWER_CALIBRATION_INVALID_ARGUMENT;
}

enum sippymodem_v34_power_calibration_result
sippymodem_v34_calibrate_line_power(
	const struct sippymodem_v34_frame_schedule *initial_schedule,
	const struct sippymodem_v34_scrambler *initial_scrambler,
	const struct sippymodem_v34_4d_encoder *initial_encoder,
	const struct sippymodem_v34_power_normalizer *normalizer,
	const struct sippymodem_v34_phase3_profile *profile,
	uint32_t sample_rate,
	struct sippymodem_v34_line_power_measurement *measurement)
{
	struct sippymodem_v34_frame_schedule schedule;
	struct sippymodem_v34_scrambler scrambler;
	struct sippymodem_v34_4d_encoder encoder;
	struct sippymodem_v34_data_carrier carrier;
	struct sippymodem_v34_preemphasis preemphasis;
	struct sippymodem_v34_line_power_measurement measured = { 0 };
	uint32_t random_state = V34_POWER_CALIBRATION_SEED;
	uint32_t frames_per_superframe;
	uint32_t frame_count;
	uint32_t warmup_frames;
	uint64_t maximum_frame_samples;
	enum sippymodem_v34_preemphasis_result preemphasis_result;

	if (!sippymodem_v34_phase3_profile_is_valid(profile) ||
	    !sippymodem_v34_frame_schedule_is_valid(initial_schedule) ||
	    !sippymodem_v34_transmit_scrambler_is_valid(
		    initial_scrambler, profile->local_role) ||
	    !sippymodem_v34_4d_encoder_is_valid(initial_encoder) ||
	    !sippymodem_v34_power_normalizer_is_valid(normalizer) ||
	    !mapping_parameters_are_equal(
		    &initial_encoder->parameters, &normalizer->parameters) ||
	    measurement == NULL)
		return SIPPYMODEM_V34_POWER_CALIBRATION_INVALID_ARGUMENT;
	schedule = *initial_schedule;
	scrambler = *initial_scrambler;
	encoder = *initial_encoder;
	if (sippymodem_v34_data_carrier_init(
		    &carrier, profile, &schedule.geometry, normalizer,
		    sample_rate) != SIPPYMODEM_V34_DATA_CARRIER_OK)
		return SIPPYMODEM_V34_POWER_CALIBRATION_INVALID_ARGUMENT;
	preemphasis_result = sippymodem_v34_preemphasis_init(
		&preemphasis, sample_rate, profile->symbol_rate_numerator,
		profile->symbol_rate_denominator,
		profile->preemphasis_filter_index);
	if (preemphasis_result == SIPPYMODEM_V34_PREEMPHASIS_NUMERIC_FAILURE)
		return SIPPYMODEM_V34_POWER_CALIBRATION_NUMERIC_FAILURE;
	if (preemphasis_result != SIPPYMODEM_V34_PREEMPHASIS_OK)
		return SIPPYMODEM_V34_POWER_CALIBRATION_INVALID_ARGUMENT;
	frames_per_superframe =
		(uint32_t)schedule.geometry.data_frames_per_superframe *
		schedule.geometry.mapping_frames_per_data_frame;
	frame_count = SIPPYMODEM_V34_POWER_CALIBRATION_SUPERFRAMES *
		frames_per_superframe;
	warmup_frames = SIPPYMODEM_V34_LINE_POWER_WARMUP_SUPERFRAMES *
		frames_per_superframe;
	maximum_frame_samples =
		(uint64_t)SIPPYMODEM_V34_SYMBOLS_PER_MAPPING_FRAME *
		(((uint64_t)sample_rate * profile->symbol_rate_denominator +
		  profile->symbol_rate_numerator - 1u) /
			 profile->symbol_rate_numerator +
		 1u);
	for (uint32_t frame = 0; frame < frame_count; ++frame) {
		struct sippymodem_v34_transmitted_mapping_frame transmitted;
		enum sippymodem_v34_power_calibration_result result;
		uint64_t frame_samples = 0;

		result = generate_mapping_frame(
			&schedule, &scrambler, &encoder,
			normalizer->nonlinear_parameter,
			normalizer->precoded_mean_squared_magnitude,
			&random_state, &transmitted);
		if (result != SIPPYMODEM_V34_POWER_CALIBRATION_OK)
			return result;
		result = carrier_result(
			sippymodem_v34_data_carrier_load_mapping_frame(
				&carrier, &transmitted));
		if (result != SIPPYMODEM_V34_POWER_CALIBRATION_OK)
			return result;
		while (!sippymodem_v34_data_carrier_needs_mapping_frame(
			       &carrier)) {
			double carrier_sample;
			double line_sample;
			bool symbol_accepted;

			if (++frame_samples > maximum_frame_samples)
				return SIPPYMODEM_V34_POWER_CALIBRATION_NUMERIC_FAILURE;
			result = carrier_result(sippymodem_v34_data_carrier_sample(
				&carrier, &carrier_sample, &symbol_accepted));
			(void)symbol_accepted;
			if (result != SIPPYMODEM_V34_POWER_CALIBRATION_OK)
				return result;
			if (sippymodem_v34_preemphasis_sample(
				    &preemphasis, carrier_sample, &line_sample) !=
			    SIPPYMODEM_V34_PREEMPHASIS_OK)
				return SIPPYMODEM_V34_POWER_CALIBRATION_NUMERIC_FAILURE;
			if (frame >= warmup_frames) {
				result = observe_line_sample(&measured, line_sample);
				if (result !=
				    SIPPYMODEM_V34_POWER_CALIBRATION_OK)
					return result;
			}
		}
	}
	if (!sippymodem_v34_line_power_measurement_is_valid(&measured))
		return SIPPYMODEM_V34_POWER_CALIBRATION_NUMERIC_FAILURE;
	*measurement = measured;
	return SIPPYMODEM_V34_POWER_CALIBRATION_OK;
}

const char *sippymodem_v34_power_calibration_result_name(
	enum sippymodem_v34_power_calibration_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_POWER_CALIBRATION_OK:
		return "ok";
	case SIPPYMODEM_V34_POWER_CALIBRATION_INVALID_ARGUMENT:
		return "invalid argument";
	case SIPPYMODEM_V34_POWER_CALIBRATION_OUT_OF_RANGE:
		return "out of range";
	case SIPPYMODEM_V34_POWER_CALIBRATION_NUMERIC_FAILURE:
		return "numeric failure";
	}
	return "unknown";
}
