// SPDX-License-Identifier: BSD-3-Clause

#include "datapumps.h"

#include "modem.h"
#include "baudot_datapump.h"
#include "fsk300_datapump.h"
#include "v8_datapump.h"
#include "v22_datapump.h"
#include "v23_datapump.h"
#include "datapump.h"

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
	if (datapumps->call_progress_registered && datapumps->fsk300_registered &&
	    datapumps->v8_registered && datapumps->v23_registered &&
	    datapumps->v22_registered &&
	    datapumps->v32_registered && datapumps->baudot_registered)
		return SIPPYMODEM_DATAPUMPS_ALREADY_STARTED;
	if (datapumps->call_progress_registered || datapumps->fsk300_registered ||
	    datapumps->v8_registered || datapumps->v23_registered ||
	    datapumps->v22_registered ||
	    datapumps->v32_registered || datapumps->baudot_registered ||
	    modem_dp_get_registered_ops(DP_CALLPROG) != NULL ||
	    modem_dp_get_registered_ops(DP_V8) != NULL ||
	    modem_dp_get_registered_ops(DP_V21) != NULL ||
	    modem_dp_get_registered_ops(DP_B103) != NULL ||
	    modem_dp_get_registered_ops(DP_V23) != NULL ||
	    modem_dp_get_registered_ops(DP_V22) != NULL ||
	    modem_dp_get_registered_ops(DP_V22BIS) != NULL ||
	    modem_dp_get_registered_ops(DP_B212) != NULL ||
	    modem_dp_get_registered_ops(DP_V32) != NULL ||
	    modem_dp_get_registered_ops(DP_BAUDOT) != NULL ||
	    modem_dp_register(DP_CALLPROG, &call_progress_operations) != 0)
		return SIPPYMODEM_DATAPUMPS_REGISTRATION_CONFLICT;
	if (sippymodem_fsk300_datapump_register() != 0) {
		(void)modem_dp_deregister(
			DP_CALLPROG, &call_progress_operations);
		return SIPPYMODEM_DATAPUMPS_REGISTRATION_CONFLICT;
	}
	if (sippymodem_v8_datapump_register() != 0) {
		(void)sippymodem_fsk300_datapump_unregister();
		(void)modem_dp_deregister(
			DP_CALLPROG, &call_progress_operations);
		return SIPPYMODEM_DATAPUMPS_REGISTRATION_CONFLICT;
	}
	if (sippymodem_v23_datapump_register() != 0) {
		(void)sippymodem_v8_datapump_unregister();
		(void)sippymodem_fsk300_datapump_unregister();
		(void)modem_dp_deregister(
			DP_CALLPROG, &call_progress_operations);
		return SIPPYMODEM_DATAPUMPS_REGISTRATION_CONFLICT;
	}
	if (sippymodem_v22_datapump_register() != 0) {
		(void)sippymodem_v23_datapump_unregister();
		(void)sippymodem_v8_datapump_unregister();
		(void)sippymodem_fsk300_datapump_unregister();
		(void)modem_dp_deregister(
			DP_CALLPROG, &call_progress_operations);
		return SIPPYMODEM_DATAPUMPS_REGISTRATION_CONFLICT;
	}
	if (sippymodem_v32_datapump_register() != 0) {
		(void)sippymodem_v22_datapump_unregister();
		(void)sippymodem_v23_datapump_unregister();
		(void)sippymodem_v8_datapump_unregister();
		(void)sippymodem_fsk300_datapump_unregister();
		(void)modem_dp_deregister(
			DP_CALLPROG, &call_progress_operations);
		return SIPPYMODEM_DATAPUMPS_REGISTRATION_CONFLICT;
	}
	if (sippymodem_baudot_datapump_register() != 0) {
		(void)sippymodem_v32_datapump_unregister();
		(void)sippymodem_v22_datapump_unregister();
		(void)sippymodem_v23_datapump_unregister();
		(void)sippymodem_v8_datapump_unregister();
		(void)sippymodem_fsk300_datapump_unregister();
		(void)modem_dp_deregister(
			DP_CALLPROG, &call_progress_operations);
		return SIPPYMODEM_DATAPUMPS_REGISTRATION_CONFLICT;
	}
	datapumps->call_progress_registered = true;
	datapumps->fsk300_registered = true;
	datapumps->v8_registered = true;
	datapumps->v23_registered = true;
	datapumps->v22_registered = true;
	datapumps->v32_registered = true;
	datapumps->baudot_registered = true;
	return SIPPYMODEM_DATAPUMPS_OK;
}

enum sippymodem_datapumps_result sippymodem_datapumps_stop(
	struct sippymodem_datapumps *datapumps)
{
	if (datapumps == NULL)
		return SIPPYMODEM_DATAPUMPS_INVALID_ARGUMENT;
	if (!datapumps->call_progress_registered && !datapumps->fsk300_registered &&
	    !datapumps->v8_registered && !datapumps->v23_registered &&
	    !datapumps->v22_registered &&
	    !datapumps->v32_registered && !datapumps->baudot_registered)
		return SIPPYMODEM_DATAPUMPS_NOT_STARTED;
	if (!datapumps->call_progress_registered || !datapumps->fsk300_registered ||
	    !datapumps->v8_registered || !datapumps->v23_registered ||
	    !datapumps->v22_registered ||
	    !datapumps->v32_registered || !datapumps->baudot_registered)
		return SIPPYMODEM_DATAPUMPS_REGISTRATION_CONFLICT;
	if (call_progress_operations.use_count != 0 ||
	    sippymodem_fsk300_datapump_is_busy() ||
	    sippymodem_v8_datapump_is_busy() ||
	    sippymodem_v23_datapump_is_busy() ||
	    sippymodem_v22_datapump_is_busy() ||
	    sippymodem_v32_datapump_is_busy() ||
	    sippymodem_baudot_datapump_is_busy())
		return SIPPYMODEM_DATAPUMPS_BUSY;
	if (sippymodem_baudot_datapump_unregister() != 0)
		return SIPPYMODEM_DATAPUMPS_REGISTRATION_CONFLICT;
	if (sippymodem_v32_datapump_unregister() != 0)
		goto restore_baudot;
	if (sippymodem_v22_datapump_unregister() != 0)
		goto restore_v32;
	if (sippymodem_v23_datapump_unregister() != 0)
		goto restore_v22;
	if (sippymodem_v8_datapump_unregister() != 0) {
		(void)sippymodem_v23_datapump_register();
		goto restore_v22;
	}
	if (sippymodem_fsk300_datapump_unregister() != 0) {
		(void)sippymodem_v8_datapump_register();
		(void)sippymodem_v23_datapump_register();
		goto restore_v22;
	}
	if (modem_dp_deregister(
		    DP_CALLPROG, &call_progress_operations) != 0) {
		(void)sippymodem_fsk300_datapump_register();
		(void)sippymodem_v8_datapump_register();
		(void)sippymodem_v23_datapump_register();
		goto restore_v22;
	}
	datapumps->call_progress_registered = false;
	datapumps->fsk300_registered = false;
	datapumps->v8_registered = false;
	datapumps->v23_registered = false;
	datapumps->v22_registered = false;
	datapumps->v32_registered = false;
	datapumps->baudot_registered = false;
	return SIPPYMODEM_DATAPUMPS_OK;
restore_v22:
	(void)sippymodem_v22_datapump_register();
restore_v32:
	(void)sippymodem_v32_datapump_register();
restore_baudot:
	(void)sippymodem_baudot_datapump_register();
	return SIPPYMODEM_DATAPUMPS_REGISTRATION_CONFLICT;
}

bool sippymodem_datapumps_are_started(
	const struct sippymodem_datapumps *datapumps)
{
	return datapumps != NULL && datapumps->call_progress_registered &&
	       datapumps->fsk300_registered && datapumps->v8_registered &&
	       datapumps->v23_registered && datapumps->v22_registered &&
	       datapumps->v32_registered &&
	       datapumps->baudot_registered;
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
