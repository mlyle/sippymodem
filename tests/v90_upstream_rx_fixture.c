// SPDX-License-Identifier: BSD-3-Clause

#include "v90_upstream_fixture.h"

#include <string.h>

static enum v90_upstream_data_result mapping_result(
	enum v34_data_mapping_result result)
{
	switch (result) {
	case V34_DATA_MAPPING_OK:
		return V90_UPSTREAM_DATA_OK;
	case V34_DATA_MAPPING_OUT_OF_RANGE:
		return V90_UPSTREAM_DATA_OUT_OF_RANGE;
	case V34_DATA_MAPPING_INVALID_ARGUMENT:
		return V90_UPSTREAM_DATA_INVALID_PARAMETERS;
	case V34_DATA_MAPPING_NUMERIC_FAILURE:
		break;
	}
	return V90_UPSTREAM_DATA_NUMERIC_FAILURE;
}

enum v90_upstream_data_result v90_upstream_rx_start_after_e(
	struct v34_data_line_receiver *receiver,
	const struct v90_phase4_e_line_rx *completed_e,
	const struct v90_mp_record *mp)
{
	struct v34_data_direction_parameters direction;
	struct v34_data_mapping mapping;
	struct v34_data_line_receiver_configuration configuration = { 0 };
	enum v90_upstream_data_result result;

	if (receiver == NULL || completed_e == NULL || mp == NULL)
		return V90_UPSTREAM_DATA_INVALID_ARGUMENT;
	if (!v90_phase4_e_line_rx_is_complete(completed_e))
		return V90_UPSTREAM_DATA_INVALID_STATE;
	result = v90_upstream_data_mode_result(v90_upstream_mode_select(
		&completed_e->cp, mp, &completed_e->transmitting_profile,
		&direction));
	if (result != V90_UPSTREAM_DATA_OK)
		return result;
	result = mapping_result(v34_data_mapping_prepare(
		&mapping, &completed_e->transmitting_profile, &direction,
		false));
	if (result != V90_UPSTREAM_DATA_OK)
		return result;
	configuration.transmitting_profile =
		completed_e->transmitting_profile;
	configuration.geometry = mapping.schedule.geometry;
	configuration.normalizer = mapping.normalizer;
	configuration.trellis_selection = direction.trellis_selection;
	configuration.expanded_shaping = direction.expanded_shaping;
	configuration.detect_rate_renegotiation = true;
	memcpy(configuration.precoder_coefficients,
	       direction.precoder_coefficients,
	       sizeof(configuration.precoder_coefficients));
	return v90_phase4_e_line_rx_start_data(
		       completed_e, receiver, &configuration) ==
		       V90_PHASE4_E_LINE_RX_OK ?
		       V90_UPSTREAM_DATA_OK :
		       V90_UPSTREAM_DATA_NUMERIC_FAILURE;
}
