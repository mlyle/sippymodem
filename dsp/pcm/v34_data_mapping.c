// SPDX-License-Identifier: BSD-3-Clause

#include "v34_data_mapping.h"

#include "v34_mapping_parameters.h"
#include "v34_power_calibration.h"
#include "v34_training_line.h"

#include <math.h>
#include <stddef.h>

static bool encoder_is_at_b1_start(const struct v34_4d_encoder *encoder)
{
	if (!v34_4d_encoder_is_valid(encoder) ||
	    encoder->previous_differential_output != 0 ||
	    encoder->convolutional_encoder.state != 0)
		return false;
	for (size_t tap = 0; tap < V34_PRECODER_TAP_COUNT; ++tap) {
		if (encoder->precoder.previous_symbols[tap].in_phase != 0 ||
		    encoder->precoder.previous_symbols[tap].quadrature != 0)
			return false;
	}
	return true;
}

bool v34_data_mapping_is_valid(
	const struct v34_data_mapping *mapping)
{
	return mapping != NULL &&
	       v34_phase3_profile_is_valid(&mapping->transmitting_profile) &&
	       v34_frame_schedule_is_valid(&mapping->schedule) &&
	       mapping->schedule.geometry.symbol_rate ==
		       mapping->transmitting_profile.symbol_rate &&
	       mapping->schedule.data_frame_in_superframe + 1u ==
		       mapping->schedule.geometry.data_frames_per_superframe &&
	       mapping->schedule.next_mapping_frame == 0 &&
	       v34_transmit_scrambler_is_valid(
		       &mapping->scrambler,
		       mapping->transmitting_profile.local_role) &&
	       mapping->scrambler.history == 0 &&
	       encoder_is_at_b1_start(&mapping->encoder) &&
	       v34_power_normalizer_is_valid(&mapping->normalizer) &&
	       v34_mapping_parameters_are_equal(
		       &mapping->encoder.parameters,
		       &mapping->normalizer.parameters) &&
	       mapping->encoder.parameters.mapping_frame_bits ==
		       mapping->schedule.geometry.high_mapping_frame_bits;
}

static enum v34_data_mapping_result geometry_result(
	enum v34_frame_schedule_result result)
{
	switch (result) {
	case V34_FRAME_SCHEDULE_OK:
		return V34_DATA_MAPPING_OK;
	case V34_FRAME_SCHEDULE_INVALID_ARGUMENT:
		return V34_DATA_MAPPING_INVALID_ARGUMENT;
	case V34_FRAME_SCHEDULE_UNSUPPORTED_SELECTION:
		return V34_DATA_MAPPING_OUT_OF_RANGE;
	case V34_FRAME_SCHEDULE_FRAME_COMPLETE:
		break;
	}
	return V34_DATA_MAPPING_NUMERIC_FAILURE;
}

static enum v34_data_mapping_result calibration_result(
	enum v34_power_calibration_result result)
{
	switch (result) {
	case V34_POWER_CALIBRATION_OK:
		return V34_DATA_MAPPING_OK;
	case V34_POWER_CALIBRATION_INVALID_ARGUMENT:
		return V34_DATA_MAPPING_INVALID_ARGUMENT;
	case V34_POWER_CALIBRATION_OUT_OF_RANGE:
		return V34_DATA_MAPPING_OUT_OF_RANGE;
	case V34_POWER_CALIBRATION_NUMERIC_FAILURE:
		return V34_DATA_MAPPING_NUMERIC_FAILURE;
	}
	return V34_DATA_MAPPING_NUMERIC_FAILURE;
}

enum v34_data_mapping_result v34_data_mapping_prepare(
	struct v34_data_mapping *mapping,
	const struct v34_phase3_profile *transmitting_profile,
	const struct v34_data_direction_parameters *parameters,
	bool auxiliary_channel_enabled)
{
	struct v34_data_mapping prepared = { 0 };
	struct v34_frame_geometry geometry;
	struct v34_mapping_parameters mapping_parameters;
	enum v34_data_mapping_result result;

	if (mapping == NULL ||
	    !v34_phase3_profile_is_valid(transmitting_profile) ||
	    !v34_data_direction_parameters_are_valid(parameters))
		return V34_DATA_MAPPING_INVALID_ARGUMENT;
	result = geometry_result(v34_frame_geometry_init(
		&geometry, transmitting_profile->symbol_rate,
		parameters->primary_data_rate_2400_bps_units,
		auxiliary_channel_enabled));
	if (result != V34_DATA_MAPPING_OK)
		return result;
	if (v34_mapping_parameters_init(
		    &mapping_parameters, &geometry,
		    parameters->expanded_shaping) !=
		    V34_MAPPING_PARAMETERS_OK ||
	    v34_b1_frame_schedule_init(
		    &prepared.schedule, &geometry) !=
		    V34_FRAME_SCHEDULE_OK ||
	    v34_transmit_scrambler_init(
		    &prepared.scrambler,
		    transmitting_profile->local_role) != V34_FRAMING_OK ||
	    v34_4d_encoder_init(
		    &prepared.encoder, &mapping_parameters,
		    parameters->precoder_coefficients,
		    parameters->trellis_selection) != V34_4D_ENCODER_OK)
		return V34_DATA_MAPPING_INVALID_ARGUMENT;
	result = calibration_result(v34_calibrate_mapping_power(
		&prepared.schedule, &prepared.scrambler, &prepared.encoder,
		parameters->nonlinear_parameter, &prepared.normalizer));
	if (result != V34_DATA_MAPPING_OK)
		return result;
	prepared.transmitting_profile = *transmitting_profile;
	if (!v34_data_mapping_is_valid(&prepared))
		return V34_DATA_MAPPING_NUMERIC_FAILURE;
	*mapping = prepared;
	return V34_DATA_MAPPING_OK;
}

enum v34_data_mapping_result v34_data_mapping_line_point_gain(
	const struct v34_data_mapping *mapping, uint32_t sample_rate,
	double *point_gain)
{
	if (!v34_data_mapping_is_valid(mapping) || sample_rate == 0 ||
	    point_gain == NULL)
		return V34_DATA_MAPPING_INVALID_ARGUMENT;
	/* E, B1, and data are one uninterrupted line signal. The trained
	 * equalizer and the transmitter therefore retain the same point scale. */
	*point_gain = 1.0;
	return V34_DATA_MAPPING_OK;
}
