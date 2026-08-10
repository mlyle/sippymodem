// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_DATAPUMPS_H
#define SIPPYMODEM_DATAPUMPS_H

#include <stdbool.h>

enum sippymodem_datapumps_result {
	SIPPYMODEM_DATAPUMPS_OK,
	SIPPYMODEM_DATAPUMPS_INVALID_ARGUMENT,
	SIPPYMODEM_DATAPUMPS_ALREADY_STARTED,
	SIPPYMODEM_DATAPUMPS_NOT_STARTED,
	SIPPYMODEM_DATAPUMPS_REGISTRATION_CONFLICT,
	SIPPYMODEM_DATAPUMPS_BUSY,
};

struct sippymodem_datapumps {
	bool call_progress_registered;
	bool fsk300_registered;
	bool v23_registered;
	bool v22_registered;
	bool baudot_registered;
};

/* Registrations are process-global; exactly one live owner is supported. */
enum sippymodem_datapumps_result sippymodem_datapumps_start(
	struct sippymodem_datapumps *datapumps);
enum sippymodem_datapumps_result sippymodem_datapumps_stop(
	struct sippymodem_datapumps *datapumps);
bool sippymodem_datapumps_are_started(
	const struct sippymodem_datapumps *datapumps);
const char *sippymodem_datapumps_result_name(
	enum sippymodem_datapumps_result result);

#endif
