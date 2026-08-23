// SPDX-License-Identifier: BSD-3-Clause

#include "v34_data_startup.h"

#include "v34_mapping_parameters.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

static bool roles_are_complementary(
	enum v34_station_role local, enum v34_station_role peer)
{
	return (local == V34_CALLING_STATION &&
		peer == V34_ANSWERING_STATION) ||
	       (local == V34_ANSWERING_STATION &&
		peer == V34_CALLING_STATION);
}

static bool mapping_matches_direction(
	const struct v34_data_mapping *mapping,
	const struct v34_data_direction_parameters *direction,
	bool auxiliary_channel_enabled)
{
	return v34_data_mapping_is_valid(mapping) &&
	       mapping->schedule.geometry.primary_data_rate_2400_bps_units ==
		       direction->primary_data_rate_2400_bps_units &&
	       mapping->schedule.geometry.auxiliary_channel_enabled ==
		       auxiliary_channel_enabled &&
	       mapping->encoder.convolutional_encoder.selection ==
		       direction->trellis_selection &&
	       mapping->encoder.parameters.expanded_shaping ==
		       direction->expanded_shaping &&
	       mapping->normalizer.nonlinear_parameter ==
		       direction->nonlinear_parameter &&
	       memcmp(mapping->encoder.precoder.coefficients,
		      direction->precoder_coefficients,
		      sizeof(direction->precoder_coefficients)) == 0;
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
	       memcmp(receive->precoder_coefficients,
		      direction->precoder_coefficients,
		      sizeof(direction->precoder_coefficients)) == 0 &&
	       isfinite(receive->transmit_point_gain) &&
	       receive->transmit_point_gain > 0.0 &&
	       isfinite(receive->equalizer_adaptation) &&
	       receive->equalizer_adaptation >= 0.0 &&
	       receive->equalizer_adaptation <= 1.0;
}

bool v34_data_startup_is_valid(
	const struct v34_data_startup *startup)
{
	return startup != NULL && v34_data_mode_is_valid(&startup->mode) &&
	       roles_are_complementary(
		       startup->transmit.transmitting_profile.local_role,
		       startup->receive.transmitting_profile.local_role) &&
	       mapping_matches_direction(
		       &startup->transmit, &startup->mode.transmit,
		       startup->mode.auxiliary_channel_enabled) &&
	       receive_matches_direction(
		       &startup->receive, &startup->mode.receive,
		       startup->mode.auxiliary_channel_enabled);
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

enum v34_data_startup_result v34_data_startup_prepare(
	struct v34_data_startup *startup,
	const struct v34_data_mode *mode,
	const struct v34_phase3_profile *transmit_profile,
	const struct v34_phase3_profile *receive_profile,
	uint32_t sample_rate, double equalizer_adaptation)
{
	struct v34_data_startup prepared = { 0 };
	struct v34_data_mapping receive_mapping;
	enum v34_data_mapping_result result;

	if (startup == NULL || !v34_data_mode_is_valid(mode) ||
	    !v34_phase3_profile_is_valid(transmit_profile) ||
	    !v34_phase3_profile_is_valid(receive_profile) ||
	    !roles_are_complementary(
		    transmit_profile->local_role, receive_profile->local_role) ||
	    sample_rate == 0 ||
	    !isfinite(equalizer_adaptation) || equalizer_adaptation < 0.0 ||
	    equalizer_adaptation > 1.0)
		return V34_DATA_STARTUP_INVALID_ARGUMENT;
	result = v34_data_mapping_prepare(
		&prepared.transmit, transmit_profile, &mode->transmit,
		mode->auxiliary_channel_enabled);
	if (result != V34_DATA_MAPPING_OK)
		return mapping_result(result);
	result = v34_data_mapping_prepare(
		&receive_mapping, receive_profile, &mode->receive,
		mode->auxiliary_channel_enabled);
	if (result != V34_DATA_MAPPING_OK)
		return mapping_result(result);
	prepared.mode = *mode;
	prepared.receive.transmitting_profile = *receive_profile;
	prepared.receive.geometry = receive_mapping.schedule.geometry;
	prepared.receive.normalizer = receive_mapping.normalizer;
	result = v34_data_mapping_line_point_gain(
		&receive_mapping, sample_rate,
		&prepared.receive.transmit_point_gain);
	if (result != V34_DATA_MAPPING_OK)
		return mapping_result(result);
	memcpy(prepared.receive.precoder_coefficients,
	       mode->receive.precoder_coefficients,
	       sizeof(prepared.receive.precoder_coefficients));
	prepared.receive.equalizer_adaptation = equalizer_adaptation;
	prepared.receive.trellis_selection = mode->receive.trellis_selection;
	prepared.receive.expanded_shaping = mode->receive.expanded_shaping;
	if (!v34_data_startup_is_valid(&prepared))
		return V34_DATA_STARTUP_NUMERIC_FAILURE;
	*startup = prepared;
	return V34_DATA_STARTUP_OK;
}
