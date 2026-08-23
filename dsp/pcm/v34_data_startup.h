// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_DATA_STARTUP_H
#define V34_DATA_STARTUP_H

#include "v34_data_line_receiver.h"
#include "v34_data_mapping.h"
#include "v34_data_mode.h"

#include <stdbool.h>
#include <stdint.h>

enum v34_data_startup_result {
	V34_DATA_STARTUP_OK,
	V34_DATA_STARTUP_INVALID_ARGUMENT,
	V34_DATA_STARTUP_OUT_OF_RANGE,
	V34_DATA_STARTUP_NUMERIC_FAILURE,
};

struct v34_data_startup {
	struct v34_data_mode mode;
	struct v34_data_mapping transmit;
	struct v34_data_line_receiver_configuration receive;
};

bool v34_data_startup_is_valid(
	const struct v34_data_startup *startup);

/* Prepare both negotiated directions before starting E. The transmit mapping
 * is ready for the local B1 handoff; the receive configuration describes the
 * peer's complementary B1 mapping at sample_rate and can be combined later
 * with the trained demodulator/equalizer handoff. Rejected calls leave the
 * destination intact. */
enum v34_data_startup_result v34_data_startup_prepare(
	struct v34_data_startup *startup,
	const struct v34_data_mode *mode,
	const struct v34_phase3_profile *transmit_profile,
	const struct v34_phase3_profile *receive_profile,
	uint32_t sample_rate, double equalizer_adaptation);

#endif /* V34_DATA_STARTUP_H */
