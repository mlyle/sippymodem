// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_DATA_MODE_H
#define V34_DATA_MODE_H

#include "v34_mp.h"
#include "v34_nonlinear.h"
#include "v34_station.h"

#include <stdbool.h>
#include <stdint.h>

enum v34_data_mode_result {
	V34_DATA_MODE_OK,
	V34_DATA_MODE_INVALID_ARGUMENT,
	V34_DATA_MODE_NO_COMMON_RATE,
};

struct v34_data_direction_parameters {
	struct v34_q14_coefficient
		precoder_coefficients[V34_PRECODER_TAP_COUNT];
	enum v34_trellis_selection trellis_selection;
	enum v34_nonlinear_parameter nonlinear_parameter;
	uint8_t primary_data_rate_2400_bps_units;
	bool expanded_shaping;
};

struct v34_data_mode {
	struct v34_data_direction_parameters transmit;
	struct v34_data_direction_parameters receive;
	bool auxiliary_channel_enabled;
	bool asymmetric_rates_enabled;
};

bool v34_data_direction_parameters_are_valid(
	const struct v34_data_direction_parameters *parameters);
bool v34_data_mode_is_valid(const struct v34_data_mode *mode);

/* Select the initial post-E data mode from the ordinary MP records. Each
 * record contains its sender's receiver requirements for the remote
 * transmitter: local transmission therefore follows the peer record, while
 * local reception follows the local record. Before the first startup MP,
 * precoder coefficients are zero; a Type 0 record leaves that zero state
 * unchanged. Retrain and rate-renegotiation updates need a separate API that
 * can retain an already active Type 0 precoder state. */
enum v34_data_mode_result v34_data_mode_select_startup(
	enum v34_station_role local_role,
	const struct v34_mp_record *local_record,
	const struct v34_mp_record *peer_record,
	struct v34_data_mode *mode);

#endif /* V34_DATA_MODE_H */
