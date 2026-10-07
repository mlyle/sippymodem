// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_STREAM_FIXTURE_H
#define V34_STREAM_FIXTURE_H

#include <v34_data_stream.h>

/* Finish normal preparation before advancing a synthetic peer's media clock. */
static inline enum v34_data_stream_result v34_prepare_stream_fixture(
	struct v34_data_stream_preparation *preparation,
	const struct v34_phase3_profile *profile,
	const struct v34_data_direction_parameters *direction,
	bool auxiliary_channel_enabled, uint32_t sample_rate)
{
	struct v34_data_stream_preparation_job job;
	enum v34_data_stream_result result = v34_data_stream_begin_preparation(
		&job, profile, direction, auxiliary_channel_enabled, sample_rate);

	if (result != V34_DATA_STREAM_OK)
		return result;
	do {
		result = v34_data_stream_step_preparation(
			&job, UINT32_MAX, preparation);
	} while (result == V34_DATA_STREAM_IN_PROGRESS);
	return result;
}

#endif
