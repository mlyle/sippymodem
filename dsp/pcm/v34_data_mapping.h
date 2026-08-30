// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_DATA_MAPPING_H
#define V34_DATA_MAPPING_H

#include "v34_4d_encoder.h"
#include "v34_data_mode.h"
#include "v34_frame_schedule.h"
#include "v34_framing.h"
#include "v34_phase3_profile.h"
#include "v34_symbol_power.h"

#include <stdbool.h>

enum v34_data_mapping_result {
	V34_DATA_MAPPING_OK,
	V34_DATA_MAPPING_INVALID_ARGUMENT,
	V34_DATA_MAPPING_OUT_OF_RANGE,
	V34_DATA_MAPPING_NUMERIC_FAILURE,
};

struct v34_data_mapping {
	struct v34_phase3_profile transmitting_profile;
	struct v34_frame_schedule schedule;
	struct v34_scrambler scrambler;
	struct v34_4d_encoder encoder;
	struct v34_power_normalizer normalizer;
};

bool v34_data_mapping_is_valid(
	const struct v34_data_mapping *mapping);

/* Prepare the first B1 frame and deterministic constellation normalization
 * for one negotiated direction. No generated calibration vector is retained.
 * Rejected calls leave the destination unchanged. */
enum v34_data_mapping_result v34_data_mapping_prepare(
	struct v34_data_mapping *mapping,
	const struct v34_phase3_profile *transmitting_profile,
	const struct v34_data_direction_parameters *parameters,
	bool auxiliary_channel_enabled);

#endif /* V34_DATA_MAPPING_H */
