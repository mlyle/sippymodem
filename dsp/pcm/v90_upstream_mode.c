// SPDX-License-Identifier: BSD-3-Clause

#include "v90_upstream_mode.h"

#include "v34_mp.h"
#include "v90_phase3.h"

#include <stddef.h>
#include <string.h>

static bool cp_is_final(const struct v90_cp_parameters *cp)
{
	return v90_cp_parameters_are_valid(cp) && cp->kind == V90_CP_DATA &&
	       cp->selected_rate_index != 0 && cp->acknowledges_peer &&
	       !cp->request_silence;
}

enum v90_upstream_mode_result v90_upstream_mode_select(
	const struct v90_cp_parameters *cp,
	const struct v90_mp_record *mp,
	const struct v34_phase3_profile *transmitting_profile,
	struct v34_data_direction_parameters *direction)
{
	struct v34_data_direction_parameters selected = { 0 };
	const struct v90_mp_parameters *common;
	uint8_t rate;

	if (cp == NULL || mp == NULL || transmitting_profile == NULL ||
	    direction == NULL)
		return V90_UPSTREAM_MODE_INVALID_ARGUMENT;
	if (!cp_is_final(cp) || !v90_mp_record_is_valid(mp) ||
	    !v90_phase3_analog_profile_is_valid(transmitting_profile))
		return V90_UPSTREAM_MODE_INVALID_PARAMETERS;
	common = v90_mp_record_common(mp);
	if (common->maximum_upstream_rate_2400_bps_units == 0)
		return V90_UPSTREAM_MODE_NO_COMMON_RATE;
	/* V.90 masks omit V.34's 2400-bit/s bit. Select the highest common
	 * enabled rate within the maximum advertised by MP (9.4.2.4/V.90). */
	rate = v34_mp_highest_enabled_rate(
		(uint16_t)((cp->upstream_rate_mask &
			    common->upstream_rate_capability_mask) << 1),
		common->maximum_upstream_rate_2400_bps_units);
	if (rate == 0)
		return V90_UPSTREAM_MODE_NO_COMMON_RATE;
	selected.trellis_selection = common->upstream_trellis;
	selected.nonlinear_parameter =
		common->upstream_nonlinear_encoding ?
			V34_NONLINEAR_THETA_5_OVER_16 :
			V34_NONLINEAR_THETA_ZERO;
	selected.primary_data_rate_2400_bps_units = rate;
	selected.expanded_shaping = common->upstream_expanded_shaping;
	if (mp->type == V90_MP_RECORD_TYPE1)
		memcpy(selected.precoder_coefficients,
		       mp->parameters.type1.precoder_coefficients,
		       sizeof(selected.precoder_coefficients));
	if (!v34_data_direction_parameters_are_valid(&selected))
		return V90_UPSTREAM_MODE_INVALID_PARAMETERS;
	*direction = selected;
	return V90_UPSTREAM_MODE_OK;
}
