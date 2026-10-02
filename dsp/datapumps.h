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
	/* Owned prefix of the family list, including incomplete rollback. */
	unsigned registered_families;
};

/* Zero-initialize before first use. Registrations are process-global with one
 * owner, which serializes registration changes and datapump lifetimes.
 * A failed start/stop may retain ownership; stop also releases a partial set. */
enum datapumps_result datapumps_start(
	struct datapumps *datapumps);
enum datapumps_result datapumps_stop(
	struct datapumps *datapumps);
bool datapumps_are_started(
	const struct datapumps *datapumps);
const char *datapumps_result_name(
	enum datapumps_result result);

#endif
