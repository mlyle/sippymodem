// SPDX-License-Identifier: BSD-3-Clause

#include "datapumps.h"

#include "modem.h"
#include "baudot_datapump.h"
#include "fsk300_datapump.h"
#include "v8_datapump.h"
#include "v22_datapump.h"
#include "v23_datapump.h"
#include "datapump.h"
#include "v34_datapump.h"
#include "v90_datapump.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct call_progress {
	struct dp dp;
};

static struct dp *create_call_progress(
	struct modem *modem, enum DP_ID id, int caller, int sample_rate,
	int max_fragment, struct dp_operations *operations);
static int delete_call_progress(struct dp *dp);
static int process_call_progress(
	struct dp *dp, void *receive, void *transmit, int count);

static struct dp_operations call_progress_operations = {
	.name = "SIP call progress",
	.create = create_call_progress,
	.delete = delete_call_progress,
	.process = process_call_progress,
};

static struct call_progress *call_progress_from_dp(
	struct dp *dp)
{
	if (dp == NULL || dp->id != DP_CALLPROG ||
	    dp->op != &call_progress_operations || dp->modem == NULL ||
	    dp->dp_data == NULL)
		return NULL;
	return dp->dp_data;
}

static struct dp *create_call_progress(
	struct modem *modem, enum DP_ID id, int caller, int sample_rate,
	int max_fragment, struct dp_operations *operations)
{
	struct call_progress *call_progress;

	if (modem == NULL || id != DP_CALLPROG || caller != 1 ||
	    sample_rate != MODEM_RATE || max_fragment != MODEM_FRAG ||
	    operations != &call_progress_operations ||
	    operations->use_count != 0)
		return NULL;
	call_progress = calloc(1, sizeof(*call_progress));
	if (call_progress == NULL)
		return NULL;
	call_progress->dp = (struct dp){
		.id = id,
		.modem = modem,
		.status = DPSTAT_OK,
		.op = operations,
		.dp_data = call_progress,
	};
	operations->use_count++;
	return &call_progress->dp;
}

static int delete_call_progress(struct dp *dp)
{
	struct call_progress *call_progress =
		call_progress_from_dp(dp);

	if (call_progress == NULL || call_progress_operations.use_count != 1)
		return -1;
	call_progress_operations.use_count--;
	call_progress->dp.dp_data = NULL;
	free(call_progress);
	return 0;
}

static int process_call_progress(
	struct dp *dp, void *receive, void *transmit, int count)
{
	if (count <= 0 || count > MODEM_FRAG || transmit == NULL)
		return DPSTAT_ERROR;
	memset(transmit, 0, (size_t)count * sizeof(int16_t));
	if (call_progress_from_dp(dp) == NULL || receive == NULL)
		return DPSTAT_ERROR;
	return DPSTAT_OK;
}

static int register_call_progress(void)
{
	return modem_dp_register(DP_CALLPROG, &call_progress_operations);
}

static int unregister_call_progress(void)
{
	return modem_dp_deregister(DP_CALLPROG, &call_progress_operations);
}

static bool call_progress_is_busy(void)
{
	return call_progress_operations.use_count != 0;
}

/* Each family changes all of its aliases or leaves them unchanged on error.
 * Registration changes and datapump lifetimes are serialized by the owner. */
static const struct {
	int (*start)(void);
	int (*stop)(void);
	bool (*is_busy)(void);
} families[] = {
	{ register_call_progress, unregister_call_progress, call_progress_is_busy },
	{ fsk300_datapump_register, fsk300_datapump_unregister, fsk300_datapump_is_busy },
	{ v8_datapump_register, v8_datapump_unregister, v8_datapump_is_busy },
	{ v23_datapump_register, v23_datapump_unregister, v23_datapump_is_busy },
	{ v22_datapump_register, v22_datapump_unregister, v22_datapump_is_busy },
	{ v32_datapump_register, v32_datapump_unregister, v32_datapump_is_busy },
	{ v34_datapump_register, v34_datapump_unregister, v34_datapump_is_busy },
	{ v90_datapump_register, v90_datapump_unregister, v90_datapump_is_busy },
	{ baudot_datapump_register, baudot_datapump_unregister, baudot_datapump_is_busy },
};

enum { FAMILY_COUNT = sizeof(families) / sizeof(families[0]) };

enum datapumps_result datapumps_start(struct datapumps *datapumps)
{
	if (datapumps == NULL)
		return DATAPUMPS_INVALID_ARGUMENT;
	if (datapumps_are_started(datapumps))
		return DATAPUMPS_ALREADY_STARTED;
	if (datapumps->registered_families != 0)
		return DATAPUMPS_REGISTRATION_CONFLICT;
	while (datapumps->registered_families < FAMILY_COUNT) {
		if (families[datapumps->registered_families].start() != 0) {
			(void)datapumps_stop(datapumps);
			return DATAPUMPS_REGISTRATION_CONFLICT;
		}
		datapumps->registered_families++;
	}
	return DATAPUMPS_OK;
}

enum datapumps_result datapumps_stop(struct datapumps *datapumps)
{
	unsigned previous_count;

	if (datapumps == NULL)
		return DATAPUMPS_INVALID_ARGUMENT;
	if (datapumps->registered_families == 0)
		return DATAPUMPS_NOT_STARTED;
	if (datapumps->registered_families > FAMILY_COUNT)
		return DATAPUMPS_REGISTRATION_CONFLICT;
	previous_count = datapumps->registered_families;
	for (unsigned i = 0; i < previous_count; ++i)
		if (families[i].is_busy())
			return DATAPUMPS_BUSY;
	while (datapumps->registered_families != 0) {
		if (families[datapumps->registered_families - 1].stop() != 0) {
			while (datapumps->registered_families < previous_count) {
				if (families[datapumps->registered_families].start() != 0)
					break;
				datapumps->registered_families++;
			}
			return DATAPUMPS_REGISTRATION_CONFLICT;
		}
		datapumps->registered_families--;
	}
	return DATAPUMPS_OK;
}

bool datapumps_are_started(const struct datapumps *datapumps)
{
	return datapumps != NULL && datapumps->registered_families == FAMILY_COUNT;
}

const char *datapumps_result_name(
	enum datapumps_result result)
{
	switch (result) {
	case DATAPUMPS_OK:
		return "ok";
	case DATAPUMPS_INVALID_ARGUMENT:
		return "invalid argument";
	case DATAPUMPS_ALREADY_STARTED:
		return "already started";
	case DATAPUMPS_NOT_STARTED:
		return "not started";
	case DATAPUMPS_REGISTRATION_CONFLICT:
		return "data-pump registration conflict";
	case DATAPUMPS_BUSY:
		return "data pump is active";
	}
	return "unknown data-pump result";
}
