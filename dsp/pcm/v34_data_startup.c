// SPDX-License-Identifier: BSD-3-Clause

#include "v34_data_startup.h"

#include "v34_mapping_parameters.h"

#include <stddef.h>
#include <string.h>

static bool mapping_matches_direction(
	const struct v34_data_mapping *mapping,
	const struct v34_data_direction_parameters *direction,
	bool auxiliary_channel_enabled)
{
	return mapping->schedule.geometry.primary_data_rate_2400_bps_units ==
		       direction->primary_data_rate_2400_bps_units &&
	       mapping->schedule.geometry.auxiliary_channel_enabled ==
		       auxiliary_channel_enabled &&
	       mapping->encoder.convolutional_encoder.selection ==
		       direction->trellis_selection &&
	       mapping->encoder.parameters.expanded_shaping ==
		       direction->expanded_shaping &&
	       mapping->normalizer.nonlinear_parameter ==
		       direction->nonlinear_parameter &&
	       v34_precoder_coefficients_are_equal(
		       mapping->encoder.precoder.coefficients,
		       direction->precoder_coefficients);
}

static bool receive_matches_direction(
	const struct v34_data_line_receiver_configuration *receive,
	const struct v34_data_direction_parameters *direction,
	bool auxiliary_channel_enabled)
{
	struct v34_mapping_parameters expected;

	return v34_phase3_profile_is_valid(&receive->transmitting_profile) &&
	       v34_frame_geometry_is_valid(&receive->geometry) &&
	       receive->geometry.symbol_rate ==
		       receive->transmitting_profile.symbol_rate &&
	       receive->geometry.primary_data_rate_2400_bps_units ==
		       direction->primary_data_rate_2400_bps_units &&
	       receive->geometry.auxiliary_channel_enabled ==
		       auxiliary_channel_enabled &&
	       v34_power_normalizer_is_valid(&receive->normalizer) &&
	       receive->normalizer.nonlinear_parameter ==
		       direction->nonlinear_parameter &&
	       v34_mapping_parameters_init(
		       &expected, &receive->geometry,
		       direction->expanded_shaping) ==
		       V34_MAPPING_PARAMETERS_OK &&
	       v34_mapping_parameters_are_equal(
		       &receive->normalizer.parameters, &expected) &&
	       receive->trellis_selection == direction->trellis_selection &&
	       receive->expanded_shaping == direction->expanded_shaping &&
	       v34_precoder_coefficients_are_equal(
		       receive->precoder_coefficients,
		       direction->precoder_coefficients);
}

bool v34_data_startup_is_valid(
	const struct v34_data_startup *startup)
{
	return startup != NULL &&
	       v34_data_stream_preparation_is_valid(&startup->transmit) &&
	       startup->receive.transmitting_profile.local_role ==
		       v34_station_peer_role(
			       startup->transmit.mapping.transmitting_profile.local_role) &&
	       v34_data_startup_matches_mode(startup, &startup->mode);
}

static enum v34_data_startup_result mapping_result(
	enum v34_data_mapping_result result)
{
	switch (result) {
	case V34_DATA_MAPPING_OK:
		return V34_DATA_STARTUP_OK;
	case V34_DATA_MAPPING_INVALID_ARGUMENT:
		return V34_DATA_STARTUP_INVALID_ARGUMENT;
	case V34_DATA_MAPPING_OUT_OF_RANGE:
		return V34_DATA_STARTUP_OUT_OF_RANGE;
	case V34_DATA_MAPPING_NUMERIC_FAILURE:
		return V34_DATA_STARTUP_NUMERIC_FAILURE;
	}
	return V34_DATA_STARTUP_NUMERIC_FAILURE;
}
bool v34_data_startup_matches_mode(
	const struct v34_data_startup *startup, const struct v34_data_mode *mode)
{
	return startup != NULL && v34_data_mode_is_valid(mode) &&
	       startup->mode.asymmetric_rates_enabled == mode->asymmetric_rates_enabled &&
	       mapping_matches_direction(&startup->transmit.mapping, &mode->transmit,
		       mode->auxiliary_channel_enabled) &&
	       receive_matches_direction(&startup->receive, &mode->receive,
		       mode->auxiliary_channel_enabled);
}

static enum v34_data_startup_result power_result(
	enum v34_power_calibration_result result)
{
	switch (result) {
	case V34_POWER_CALIBRATION_OK: return V34_DATA_STARTUP_OK;
	case V34_POWER_CALIBRATION_IN_PROGRESS: return V34_DATA_STARTUP_IN_PROGRESS;
	case V34_POWER_CALIBRATION_INVALID_ARGUMENT: return V34_DATA_STARTUP_INVALID_ARGUMENT;
	case V34_POWER_CALIBRATION_OUT_OF_RANGE: return V34_DATA_STARTUP_OUT_OF_RANGE;
	case V34_POWER_CALIBRATION_NUMERIC_FAILURE: return V34_DATA_STARTUP_NUMERIC_FAILURE;
	}
	return V34_DATA_STARTUP_NUMERIC_FAILURE;
}

enum v34_data_startup_result v34_data_startup_begin_preparation(
	struct v34_data_startup_preparation *preparation,
	const struct v34_data_mode *mode,
	const struct v34_phase3_profile *transmit_profile,
	const struct v34_phase3_profile *receive_profile, uint32_t sample_rate)
{
	struct v34_data_startup_preparation prepared = { 0 };
	enum v34_data_mapping_result result;

	if (preparation == NULL || sample_rate == 0 || !v34_data_mode_is_valid(mode) ||
	    !v34_phase3_profile_is_valid(transmit_profile) ||
	    !v34_phase3_profile_is_valid(receive_profile) ||
	    receive_profile->local_role != v34_station_peer_role(transmit_profile->local_role))
		return V34_DATA_STARTUP_INVALID_ARGUMENT;
	result = v34_data_mapping_begin_calibration(&prepared.receive_mapping,
		&prepared.calibration.receive, receive_profile, &mode->receive,
		mode->auxiliary_channel_enabled);
	if (result != V34_DATA_MAPPING_OK)
		return mapping_result(result);
	switch (v34_data_stream_begin_preparation(&prepared.calibration.transmit,
		transmit_profile, &mode->transmit, mode->auxiliary_channel_enabled, sample_rate)) {
	case V34_DATA_STREAM_OK: break;
	case V34_DATA_STREAM_OUT_OF_RANGE: return V34_DATA_STARTUP_OUT_OF_RANGE;
	case V34_DATA_STREAM_INVALID_ARGUMENT: return V34_DATA_STARTUP_INVALID_ARGUMENT;
	default: return V34_DATA_STARTUP_NUMERIC_FAILURE;
	}
	prepared.startup.mode = *mode;
	prepared.startup.transmit.sample_rate = sample_rate;
	*preparation = prepared;
	return V34_DATA_STARTUP_OK;
}

enum v34_data_startup_result v34_data_startup_step_preparation(
	struct v34_data_startup_preparation *preparation,
	uint32_t frame_budget, struct v34_data_startup *startup)
{
	enum v34_power_calibration_result result;
	struct v34_data_mapping *receive;

	if (preparation == NULL || startup == NULL || frame_budget == 0)
		return V34_DATA_STARTUP_INVALID_ARGUMENT;
	receive = &preparation->receive_mapping;
	switch (preparation->stage) {
	case V34_DATA_PREPARATION_TRANSMIT_MAPPING:
	case V34_DATA_PREPARATION_TRANSMIT_LINE:
		{
			enum v34_data_stream_result step = v34_data_stream_step_preparation(
				&preparation->calibration.transmit, frame_budget,
				&preparation->startup.transmit);
			if (step == V34_DATA_STREAM_IN_PROGRESS) {
				preparation->stage = preparation->calibration.transmit.calibrating_line ?
					V34_DATA_PREPARATION_TRANSMIT_LINE : V34_DATA_PREPARATION_TRANSMIT_MAPPING;
				return V34_DATA_STARTUP_IN_PROGRESS;
			}
			if (step != V34_DATA_STREAM_OK)
				return step == V34_DATA_STREAM_OUT_OF_RANGE ? V34_DATA_STARTUP_OUT_OF_RANGE :
					V34_DATA_STARTUP_NUMERIC_FAILURE;
		}
		result = v34_mapping_power_calibration_begin(&preparation->calibration.receive,
			&receive->schedule, &receive->scrambler, &receive->encoder,
			preparation->startup.mode.receive.nonlinear_parameter);
		if (result != V34_POWER_CALIBRATION_OK)
			return power_result(result);
		preparation->stage = V34_DATA_PREPARATION_RECEIVE_MAPPING;
		return V34_DATA_STARTUP_IN_PROGRESS;
	case V34_DATA_PREPARATION_RECEIVE_MAPPING:
		result = v34_mapping_power_calibration_step(&preparation->calibration.receive,
			frame_budget, &receive->normalizer);
		if (result != V34_POWER_CALIBRATION_OK)
			return power_result(result);
		preparation->startup.receive = (struct v34_data_line_receiver_configuration){
			.transmitting_profile = receive->transmitting_profile,
			.geometry = receive->schedule.geometry,
			.normalizer = receive->normalizer,
			.trellis_selection = preparation->startup.mode.receive.trellis_selection,
			.expanded_shaping = preparation->startup.mode.receive.expanded_shaping,
		};
		memcpy(preparation->startup.receive.precoder_coefficients,
			preparation->startup.mode.receive.precoder_coefficients,
			sizeof(preparation->startup.receive.precoder_coefficients));
		preparation->stage = V34_DATA_PREPARATION_COMPLETE;
		break;
	case V34_DATA_PREPARATION_COMPLETE:
		break;
	default:
		return V34_DATA_STARTUP_INVALID_ARGUMENT;
	}
	if (!v34_data_startup_is_valid(&preparation->startup))
		return V34_DATA_STARTUP_NUMERIC_FAILURE;
	*startup = preparation->startup;
	return V34_DATA_STARTUP_OK;
}
