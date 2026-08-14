// SPDX-License-Identifier: BSD-3-Clause

#ifndef DATAPUMPS_H
#define DATAPUMPS_H

#include <stdbool.h>

enum datapumps_result {
	DATAPUMPS_OK,
	DATAPUMPS_INVALID_ARGUMENT,
	DATAPUMPS_ALREADY_STARTED,
	DATAPUMPS_NOT_STARTED,
	DATAPUMPS_REGISTRATION_CONFLICT,
	DATAPUMPS_BUSY,
};

struct datapumps {
	bool call_progress_registered;
	bool fsk300_registered;
	bool v8_registered;
	bool v23_registered;
	bool v22_registered;
	bool v32_registered;
	bool v34_registered;
	bool v90_registered;
	bool baudot_registered;
};

/* Registrations are process-global; exactly one live owner is supported. */
enum datapumps_result datapumps_start(
	struct datapumps *datapumps);
enum datapumps_result datapumps_stop(
	struct datapumps *datapumps);
bool datapumps_are_started(
	const struct datapumps *datapumps);
const char *datapumps_result_name(
	enum datapumps_result result);

#endif
