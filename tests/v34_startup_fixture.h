// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_STARTUP_FIXTURE_H
#define V34_STARTUP_FIXTURE_H

#include <v34_data_startup.h>

/* Finish normal preparation before advancing a synthetic peer's media clock. */
static inline bool v34_prepare_startup_fixture(
	struct v34_data_startup *startup, const struct v34_data_mode *mode,
	const struct v34_phase3_profile *transmit_profile,
	const struct v34_phase3_profile *receive_profile, uint32_t sample_rate)
{
	struct v34_data_startup_preparation job;
	enum v34_data_startup_result result;

	if (v34_data_startup_begin_preparation(&job, mode, transmit_profile,
		    receive_profile, sample_rate) != V34_DATA_STARTUP_OK)
		return false;
	do {
		result = v34_data_startup_step_preparation(&job, UINT32_MAX, startup);
	} while (result == V34_DATA_STARTUP_IN_PROGRESS);
	return result == V34_DATA_STARTUP_OK;
}

#endif
