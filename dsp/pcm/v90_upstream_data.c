// SPDX-License-Identifier: BSD-3-Clause

#include "v90_upstream_data.h"

#include <stddef.h>
#include <string.h>

static const struct v90_mp_parameters *common_parameters(
	const struct v90_mp_record *record)
{
	if (record->type == V90_MP_RECORD_TYPE0)
		return &record->parameters.type0;
	return &record->parameters.type1.common;
}

static uint8_t select_rate(
	const struct v90_cp_parameters *cp,
	const struct v90_mp_parameters *mp,
	const struct v34_phase3_profile *profile)
{
	uint8_t maximum = mp->maximum_upstream_rate_2400_bps_units;

	if (maximum > profile->projected_data_rate_2400_bps_units)
		maximum = profile->projected_data_rate_2400_bps_units;
	for (int rate = maximum;
	     rate >= V90_MP_MINIMUM_RATE_2400_BPS_UNITS; --rate) {
		uint16_t bit = (uint16_t)(1u <<
			(rate - V90_MP_MINIMUM_RATE_2400_BPS_UNITS));

		if ((cp->upstream_rate_mask & bit) != 0 &&
		    (mp->upstream_rate_capability_mask & bit) != 0)
			return (uint8_t)rate;
	}
	return 0;
}

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
	case V34_DATA_STREAM_NUMERIC_FAILURE:
	case V34_DATA_STREAM_SOURCE_FAILURE:
		return V90_UPSTREAM_DATA_NUMERIC_FAILURE;
	}
	return V90_UPSTREAM_DATA_NUMERIC_FAILURE;
}

enum v90_upstream_data_result v90_upstream_data_start_after_e(
	struct v34_data_stream *stream,
	const struct v90_phase4_e_line_tx *completed_e,
	const struct v90_mp_record *peer_mp)
{
	struct v34_data_stream_configuration configuration = { 0 };
	const struct v90_mp_parameters *mp;
	uint8_t rate;

	if (stream == NULL || completed_e == NULL || peer_mp == NULL)
		return V90_UPSTREAM_DATA_INVALID_ARGUMENT;
	if (!v90_phase4_e_line_tx_is_complete(completed_e))
		return V90_UPSTREAM_DATA_INVALID_STATE;
	if (!v90_mp_record_is_valid(peer_mp))
		return V90_UPSTREAM_DATA_INVALID_PARAMETERS;
	mp = common_parameters(peer_mp);
	if (mp->maximum_upstream_rate_2400_bps_units == 0)
		return V90_UPSTREAM_DATA_NO_COMMON_RATE;
	rate = select_rate(&completed_e->cp, mp, &completed_e->line.profile);
	if (rate == 0)
		return V90_UPSTREAM_DATA_NO_COMMON_RATE;
	configuration.profile = completed_e->line.profile;
	configuration.sample_rate =
		completed_e->line.carrier.channel.sample_rate;
	configuration.nominal_peak_amplitude =
		completed_e->line.nominal_peak_amplitude;
	configuration.trellis_selection = mp->upstream_trellis;
	configuration.nonlinear_parameter =
		mp->upstream_nonlinear_encoding ?
			V34_NONLINEAR_THETA_5_OVER_16 :
			V34_NONLINEAR_THETA_ZERO;
	configuration.primary_data_rate_2400_bps_units = rate;
	configuration.expanded_shaping = mp->upstream_expanded_shaping;
	if (peer_mp->type == V90_MP_RECORD_TYPE1)
		memcpy(configuration.precoder_coefficients,
		       peer_mp->parameters.type1.precoder_coefficients,
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
