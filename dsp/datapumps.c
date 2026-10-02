// SPDX-License-Identifier: BSD-3-Clause

#include "datapumps.h"

#include "modem.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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

static bool call_progress_is_valid(const struct dp *dp)
{
	return dp != NULL && dp->id == DP_CALLPROG &&
	       dp->op == &call_progress_operations && dp->modem != NULL &&
	       dp->dp_data == dp;
}

static struct dp *create_call_progress(
	struct modem *modem, enum DP_ID id, int caller, int sample_rate,
	int max_fragment, struct dp_operations *operations)
{
	struct dp *call_progress;

	if (modem == NULL || id != DP_CALLPROG || caller != 1 ||
	    sample_rate != MODEM_RATE || max_fragment != MODEM_FRAG ||
	    operations != &call_progress_operations ||
	    !modem_dp_has_capacity(operations))
		return NULL;
	call_progress = calloc(1, sizeof(*call_progress));
	if (call_progress == NULL)
		return NULL;
	*call_progress = (struct dp){
		.id = id,
		.modem = modem,
		.status = DPSTAT_OK,
		.op = operations,
		.dp_data = call_progress,
	};
	operations->use_count++;
	return call_progress;
}

static int delete_call_progress(struct dp *dp)
{
	if (!call_progress_is_valid(dp) || call_progress_operations.use_count <= 0)
		return -1;
	call_progress_operations.use_count--;
	free(dp);
	return 0;
}

static int process_call_progress(
	struct dp *dp, void *receive, void *transmit, int count)
{
	if (count <= 0 || count > MODEM_FRAG || transmit == NULL)
		return DPSTAT_ERROR;
	memset(transmit, 0, (size_t)count * sizeof(int16_t));
	if (!call_progress_is_valid(dp) || receive == NULL)
		return DPSTAT_ERROR;
	return DPSTAT_OK;
}

struct dp_operations *call_progress_datapump_operations(enum DP_ID id)
{
	return id == DP_CALLPROG ? &call_progress_operations : NULL;
}
