// SPDX-License-Identifier: BSD-3-Clause

#include "v34_data_mode.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

static bool trellis_is_valid(enum v34_trellis_selection selection)
{
	return selection == V34_TRELLIS_16_STATE ||
	       selection == V34_TRELLIS_32_STATE ||
	       selection == V34_TRELLIS_64_STATE;
}

static bool nonlinear_is_valid(enum v34_nonlinear_parameter parameter)
{
	return parameter == V34_NONLINEAR_THETA_ZERO ||
	       parameter == V34_NONLINEAR_THETA_5_OVER_16;
}

bool v34_data_direction_parameters_are_valid(
	const struct v34_data_direction_parameters *parameters)
{
	if (parameters == NULL ||
	    parameters->primary_data_rate_2400_bps_units <
		    V34_MP_MINIMUM_RATE_2400_BPS_UNITS ||
	    parameters->primary_data_rate_2400_bps_units >
		    V34_MP_MAXIMUM_RATE_2400_BPS_UNITS ||
	    !trellis_is_valid(parameters->trellis_selection) ||
	    !nonlinear_is_valid(parameters->nonlinear_parameter))
		return false;
	for (size_t index = 0; index < V34_PRECODER_TAP_COUNT; ++index) {
		if (parameters->precoder_coefficients[index].real == INT16_MIN ||
		    parameters->precoder_coefficients[index].imaginary ==
			    INT16_MIN)
			return false;
	}
	return true;
}

bool v34_data_mode_is_valid(const struct v34_data_mode *mode)
{
	return mode != NULL &&
	       v34_data_direction_parameters_are_valid(&mode->transmit) &&
	       v34_data_direction_parameters_are_valid(&mode->receive) &&
	       (mode->asymmetric_rates_enabled ||
		mode->transmit.primary_data_rate_2400_bps_units ==
			mode->receive.primary_data_rate_2400_bps_units);
}

static void direction_from_record(
	const struct v34_mp_record *record, uint8_t rate,
	struct v34_data_direction_parameters *direction)
{
	const struct v34_mp_parameters *common =
		v34_mp_record_common_parameters(record);
	struct v34_data_direction_parameters selected = {
		.trellis_selection = common->remote_transmitter_trellis,
		.nonlinear_parameter =
			common->remote_transmitter_nonlinear_encoding ?
				V34_NONLINEAR_THETA_5_OVER_16 :
				V34_NONLINEAR_THETA_ZERO,
		.primary_data_rate_2400_bps_units = rate,
		.expanded_shaping =
			common->remote_transmitter_expanded_shaping,
	};

	if (record->type == V34_MP_RECORD_TYPE1)
		memcpy(selected.precoder_coefficients,
		       record->parameters.type1.precoder_coefficients,
		       sizeof(selected.precoder_coefficients));
	*direction = selected;
}

static enum v34_data_mode_result rate_result(
	enum v34_mp_rate_result result)
{
	switch (result) {
	case V34_MP_RATE_OK:
		return V34_DATA_MODE_OK;
	case V34_MP_RATE_INVALID_ARGUMENT:
		return V34_DATA_MODE_INVALID_ARGUMENT;
	case V34_MP_RATE_NO_COMMON_RATE:
		return V34_DATA_MODE_NO_COMMON_RATE;
	}
	return V34_DATA_MODE_INVALID_ARGUMENT;
}

enum v34_data_mode_result v34_data_mode_select_startup(
	enum v34_station_role local_role,
	const struct v34_mp_record *local_record,
	const struct v34_mp_record *peer_record,
	struct v34_data_mode *mode)
{
	struct v34_mp_rates rates;
	struct v34_data_mode selected = { 0 };
	enum v34_mp_rate_result result;
	uint8_t transmit_rate;
	uint8_t receive_rate;

	if (!v34_station_role_is_valid(local_role) || mode == NULL)
		return V34_DATA_MODE_INVALID_ARGUMENT;
	result = v34_mp_select_rates(local_record, peer_record, &rates);
	if (result != V34_MP_RATE_OK)
		return rate_result(result);
	if (local_role == V34_CALLING_STATION) {
		transmit_rate = rates.call_to_answer_2400_bps_units;
		receive_rate = rates.answer_to_call_2400_bps_units;
	} else {
		transmit_rate = rates.answer_to_call_2400_bps_units;
		receive_rate = rates.call_to_answer_2400_bps_units;
	}
	direction_from_record(peer_record, transmit_rate, &selected.transmit);
	direction_from_record(local_record, receive_rate, &selected.receive);
	selected.auxiliary_channel_enabled = rates.auxiliary_channel_enabled;
	selected.asymmetric_rates_enabled = rates.asymmetric_rates_enabled;
	if (!v34_data_mode_is_valid(&selected))
		return V34_DATA_MODE_INVALID_ARGUMENT;
	*mode = selected;
	return V34_DATA_MODE_OK;
}
