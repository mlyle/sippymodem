// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_datapumps.h"

#include "modem.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct sippymodem_call_progress {
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

static struct sippymodem_call_progress *call_progress_from_dp(
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
	struct sippymodem_call_progress *call_progress;

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
	struct sippymodem_call_progress *call_progress =
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

enum sippymodem_datapumps_result sippymodem_datapumps_start(
	struct sippymodem_datapumps *datapumps)
{
	if (datapumps == NULL)
		return SIPPYMODEM_DATAPUMPS_INVALID_ARGUMENT;
	if (datapumps->call_progress_registered)
		return SIPPYMODEM_DATAPUMPS_ALREADY_STARTED;
	if (modem_dp_get_registered_ops(DP_CALLPROG) != NULL ||
	    modem_dp_register(DP_CALLPROG, &call_progress_operations) != 0)
		return SIPPYMODEM_DATAPUMPS_REGISTRATION_CONFLICT;
	datapumps->call_progress_registered = true;
	return SIPPYMODEM_DATAPUMPS_OK;
}

enum sippymodem_datapumps_result sippymodem_datapumps_stop(
	struct sippymodem_datapumps *datapumps)
{
	if (datapumps == NULL)
		return SIPPYMODEM_DATAPUMPS_INVALID_ARGUMENT;
	if (!datapumps->call_progress_registered)
		return SIPPYMODEM_DATAPUMPS_NOT_STARTED;
	if (call_progress_operations.use_count != 0)
		return SIPPYMODEM_DATAPUMPS_BUSY;
	if (modem_dp_deregister(
		    DP_CALLPROG, &call_progress_operations) != 0)
		return SIPPYMODEM_DATAPUMPS_REGISTRATION_CONFLICT;
	datapumps->call_progress_registered = false;
	return SIPPYMODEM_DATAPUMPS_OK;
}

bool sippymodem_datapumps_are_started(
	const struct sippymodem_datapumps *datapumps)
{
	return datapumps != NULL && datapumps->call_progress_registered;
}

const char *sippymodem_datapumps_result_name(
	enum sippymodem_datapumps_result result)
{
	switch (result) {
	case SIPPYMODEM_DATAPUMPS_OK:
		return "ok";
	case SIPPYMODEM_DATAPUMPS_INVALID_ARGUMENT:
		return "invalid argument";
	case SIPPYMODEM_DATAPUMPS_ALREADY_STARTED:
		return "already started";
	case SIPPYMODEM_DATAPUMPS_NOT_STARTED:
		return "not started";
	case SIPPYMODEM_DATAPUMPS_REGISTRATION_CONFLICT:
		return "data-pump registration conflict";
	case SIPPYMODEM_DATAPUMPS_BUSY:
		return "data pump is active";
	}
	return "unknown data-pump result";
}
