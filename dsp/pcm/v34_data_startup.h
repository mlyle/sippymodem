// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_DATA_STARTUP_H
#define V34_DATA_STARTUP_H

#include "v34_data_line_receiver.h"
#include "v34_data_stream.h"
#include "v34_data_mode.h"

#include <stdbool.h>

enum v34_data_startup_result {
	V34_DATA_STARTUP_OK,
	V34_DATA_STARTUP_INVALID_ARGUMENT,
	V34_DATA_STARTUP_OUT_OF_RANGE,
	V34_DATA_STARTUP_NUMERIC_FAILURE,
	V34_DATA_STARTUP_IN_PROGRESS,
};

struct v34_data_startup {
	struct v34_data_mode mode;
	struct v34_data_stream_preparation transmit;
	struct v34_data_line_receiver_configuration receive;
};

enum v34_data_preparation_stage {
	V34_DATA_PREPARATION_TRANSMIT_MAPPING,
	V34_DATA_PREPARATION_TRANSMIT_LINE,
	V34_DATA_PREPARATION_RECEIVE_MAPPING,
	V34_DATA_PREPARATION_COMPLETE,
};

/* Kept outside the phase state copied on every line sample. Only calibration
 * history is retained; the deterministic generated stream is never stored. */
struct v34_data_startup_preparation {
	struct v34_data_startup startup;
	struct v34_data_mapping receive_mapping;
	union {
		struct v34_data_stream_preparation_job transmit;
		struct v34_mapping_power_calibration receive;
	} calibration;
	enum v34_data_preparation_stage stage;
};

/* Prepare complementary B1 mappings and transmit line power before E.
 * The trained receive line attaches later; rejected begin calls leave the job intact. */
enum v34_data_startup_result v34_data_startup_begin_preparation(
	struct v34_data_startup_preparation *preparation,
	const struct v34_data_mode *mode,
	const struct v34_phase3_profile *transmit_profile,
	const struct v34_phase3_profile *receive_profile, uint32_t sample_rate);
/* At most frame_budget generated mapping frames in one calibration phase.
 * IN_PROGRESS leaves startup intact. Discard the job on terminal failure. */
enum v34_data_startup_result v34_data_startup_step_preparation(
	struct v34_data_startup_preparation *preparation,
	uint32_t frame_budget, struct v34_data_startup *startup);
bool v34_data_startup_matches_mode(
	const struct v34_data_startup *startup, const struct v34_data_mode *mode);

bool v34_data_startup_is_valid(
	const struct v34_data_startup *startup);

#endif /* V34_DATA_STARTUP_H */
