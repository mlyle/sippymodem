// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_MAPPING_FIXTURE_H
#define V34_MAPPING_FIXTURE_H

#include <v34_data_mapping.h>

/* Finish a valid synthetic mapping before advancing the peer's media clock.
 * Test rejected inputs directly through the bounded constructor. */
static inline enum v34_data_mapping_result v34_prepare_mapping_fixture(
	struct v34_data_mapping *mapping,
	const struct v34_phase3_profile *profile,
	const struct v34_data_direction_parameters *direction,
	bool auxiliary_channel_enabled)
{
	struct v34_mapping_power_calibration job;
	enum v34_data_mapping_result result = v34_data_mapping_begin_calibration(
		mapping, &job, profile, direction, auxiliary_channel_enabled);

	if (result != V34_DATA_MAPPING_OK)
		return result;
	enum v34_power_calibration_result calibrated = v34_mapping_power_calibration_step(
		&job, UINT32_MAX, &mapping->normalizer);

	if (calibrated == V34_POWER_CALIBRATION_INVALID_ARGUMENT)
		return V34_DATA_MAPPING_INVALID_ARGUMENT;
	if (calibrated == V34_POWER_CALIBRATION_OUT_OF_RANGE)
		return V34_DATA_MAPPING_OUT_OF_RANGE;
	return calibrated == V34_POWER_CALIBRATION_OK && v34_data_mapping_is_valid(mapping) ?
		       V34_DATA_MAPPING_OK : V34_DATA_MAPPING_NUMERIC_FAILURE;
}

#endif
