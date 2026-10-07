// SPDX-License-Identifier: BSD-3-Clause
#ifndef V34_STARTUP_STATE_FIXTURE_H
#define V34_STARTUP_STATE_FIXTURE_H

#include <v34_data_startup.h>
#include "v34_state_fixture.h"

static inline bool v34_data_modes_equal(
	const struct v34_data_mode *left, const struct v34_data_mode *right)
{
	for (unsigned int direction = 0; direction < 2; ++direction) {
		const struct v34_data_direction_parameters *a =
			direction ? &left->receive : &left->transmit;
		const struct v34_data_direction_parameters *b =
			direction ? &right->receive : &right->transmit;

		if (!v34_precoder_coefficients_are_equal(a->precoder_coefficients,
			    b->precoder_coefficients) ||
		    a->trellis_selection != b->trellis_selection ||
		    a->nonlinear_parameter != b->nonlinear_parameter ||
		    a->primary_data_rate_2400_bps_units != b->primary_data_rate_2400_bps_units ||
		    a->expanded_shaping != b->expanded_shaping)
			return false;
	}
	return left->auxiliary_channel_enabled == right->auxiliary_channel_enabled &&
	       left->asymmetric_rates_enabled == right->asymmetric_rates_enabled;
}

static inline bool v34_data_startups_equal(
	const struct v34_data_startup *left, const struct v34_data_startup *right)
{
	struct v34_data_startup snapshots[2];

	if (!v34_data_modes_equal(&left->mode, &right->mode) ||
	    !v34_phase3_profile_is_equal(&left->transmit.mapping.transmitting_profile,
		    &right->transmit.mapping.transmitting_profile) ||
	    !v34_phase3_profile_is_equal(&left->receive.transmitting_profile,
		    &right->receive.transmitting_profile))
		return false;
	memcpy(&snapshots[0], left, sizeof(*left));
	memcpy(&snapshots[1], right, sizeof(*right));
	for (size_t index = 0; index < 2; ++index) {
		struct v34_data_startup *startup = &snapshots[index];
		struct v34_data_mapping *mapping = &startup->transmit.mapping;

		/* Mode and profiles were compared above; normalize only snapshot copies. */
		memset(&startup->mode, 0, sizeof(startup->mode));
		memset(&mapping->transmitting_profile, 0, sizeof(mapping->transmitting_profile));
		memset(&startup->receive.transmitting_profile, 0,
		       sizeof(startup->receive.transmitting_profile));
		v34_snapshot_geometry(&mapping->schedule.geometry);
		v34_snapshot_parameters(&mapping->encoder.parameters);
		v34_snapshot_parameters(&mapping->normalizer.parameters);
		v34_snapshot_geometry(&startup->receive.geometry);
		v34_snapshot_parameters(&startup->receive.normalizer.parameters);
	}
	return memcmp(&snapshots[0], &snapshots[1], sizeof(*left)) == 0;
}

#endif
