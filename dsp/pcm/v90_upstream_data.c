// SPDX-License-Identifier: BSD-3-Clause

#include "v90_upstream_data.h"
#include "v90_upstream_mode.h"

#include <stddef.h>
#include <string.h>

static enum v90_upstream_data_result stream_result(
	enum v34_data_stream_result result)
{
	switch (result) {
	case V34_DATA_STREAM_OK:
		return V90_UPSTREAM_DATA_OK;
	case V34_DATA_STREAM_INVALID_ARGUMENT:
		return V90_UPSTREAM_DATA_INVALID_ARGUMENT;
	case V34_DATA_STREAM_OUT_OF_RANGE:
		return V90_UPSTREAM_DATA_OUT_OF_RANGE;
	case V34_DATA_STREAM_CLIPPING_RISK:
		return V90_UPSTREAM_DATA_CLIPPING_RISK;
	case V34_DATA_STREAM_IN_PROGRESS:
	case V34_DATA_STREAM_NUMERIC_FAILURE:
	case V34_DATA_STREAM_SOURCE_FAILURE:
		return V90_UPSTREAM_DATA_NUMERIC_FAILURE;
	}
	return V90_UPSTREAM_DATA_NUMERIC_FAILURE;
}

static enum v90_upstream_data_result mode_result(
	enum v90_upstream_mode_result result)
{
	switch (result) {
	case V90_UPSTREAM_MODE_OK:
		return V90_UPSTREAM_DATA_OK;
	case V90_UPSTREAM_MODE_INVALID_ARGUMENT:
		return V90_UPSTREAM_DATA_INVALID_ARGUMENT;
	case V90_UPSTREAM_MODE_INVALID_PARAMETERS:
		return V90_UPSTREAM_DATA_INVALID_PARAMETERS;
	case V90_UPSTREAM_MODE_NO_COMMON_RATE:
		return V90_UPSTREAM_DATA_NO_COMMON_RATE;
	}
	return V90_UPSTREAM_DATA_NUMERIC_FAILURE;
}

enum v90_upstream_data_result v90_upstream_data_start_after_e(
	struct v34_data_stream *stream,
	const struct v90_phase4_e_line_tx *completed_e,
	const struct v90_mp_record *peer_mp)
{
	struct v34_data_stream_configuration configuration = { 0 };
	struct v34_data_direction_parameters direction;
	enum v90_upstream_data_result result;

	if (stream == NULL || completed_e == NULL || peer_mp == NULL)
		return V90_UPSTREAM_DATA_INVALID_ARGUMENT;
	if (!v90_phase4_e_line_tx_is_complete(completed_e))
		return V90_UPSTREAM_DATA_INVALID_STATE;
	result = mode_result(v90_upstream_mode_select(
		&completed_e->cp, peer_mp, &completed_e->line.profile,
		&direction));
	if (result != V90_UPSTREAM_DATA_OK)
		return result;
	configuration.profile = completed_e->line.profile;
	configuration.sample_rate =
		completed_e->line.carrier.channel.sample_rate;
	configuration.nominal_peak_amplitude =
		completed_e->line.nominal_peak_amplitude;
	configuration.trellis_selection = direction.trellis_selection;
	configuration.nonlinear_parameter = direction.nonlinear_parameter;
	configuration.primary_data_rate_2400_bps_units =
		direction.primary_data_rate_2400_bps_units;
	configuration.expanded_shaping = direction.expanded_shaping;
	memcpy(configuration.precoder_coefficients,
	       direction.precoder_coefficients,
	       sizeof(configuration.precoder_coefficients));
	return stream_result(v34_data_stream_continue_b1(
		stream, &completed_e->line, &configuration));
}

const char *v90_upstream_data_result_name(
	enum v90_upstream_data_result result)
{
	switch (result) {
	case V90_UPSTREAM_DATA_OK:
		return "ok";
	case V90_UPSTREAM_DATA_INVALID_ARGUMENT:
		return "invalid argument";
	case V90_UPSTREAM_DATA_INVALID_STATE:
		return "invalid state";
	case V90_UPSTREAM_DATA_INVALID_PARAMETERS:
		return "invalid parameters";
	case V90_UPSTREAM_DATA_NO_COMMON_RATE:
		return "no common rate";
	case V90_UPSTREAM_DATA_OUT_OF_RANGE:
		return "out of range";
	case V90_UPSTREAM_DATA_CLIPPING_RISK:
		return "clipping risk";
	case V90_UPSTREAM_DATA_NUMERIC_FAILURE:
		return "numeric failure";
	}
	return "unknown";
}
