// SPDX-License-Identifier: BSD-3-Clause

#include "v8_datapump.h"

#include "modem.h"
#include "modem_param.h"
#include "v8_answering.h"
#include "v8_calling.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct v8_datapump {
	struct dp dp;
	union {
		struct v8_calling calling;
		struct v8_answering answering;
	} handshake;
	enum DP_ID requested_datapump;
	enum v8_modulation negotiated_modulation;
	bool caller;
	bool handoff_requested;
};

static struct dp *create_v8(
	struct modem *modem, enum DP_ID id, int caller, int sample_rate,
	int max_fragment, struct dp_operations *operations);
static int delete_v8(struct dp *dp);
static int process_v8(
	struct dp *dp, void *receive, void *transmit, int count);

static struct dp_operations v8_operations = {
	.name = "V.8 negotiation",
	.create = create_v8,
	.delete = delete_v8,
	.process = process_v8,
};

static struct v8_datapump *v8_from_dp(struct dp *dp)
{
	struct v8_datapump *datapump;
	enum v8_modulation modulation;

	if (dp == NULL || dp->id != DP_V8 || dp->op != &v8_operations ||
	    dp->modem == NULL || dp->dp_data == NULL)
		return NULL;
	datapump = dp->dp_data;
	modulation = v8_datapump_modulation(
		datapump->requested_datapump);
	if (&datapump->dp != dp ||
	    modulation == V8_MODULATION_NONE ||
	    modulation != datapump->negotiated_modulation)
		return NULL;
	return datapump;
}

static struct dp *create_v8(
	struct modem *modem, enum DP_ID id, int caller, int sample_rate,
	int max_fragment, struct dp_operations *operations)
{
	struct v8_datapump *datapump;
	struct v8_menu local_menu;
	enum v8_modulation modulation =
		v8_datapump_modulation(id);
	enum v8_menu_result menu_result;
	bool digital;
	bool lapm;

	if (modem == NULL || (caller != 0 && caller != 1) ||
	    sample_rate != MODEM_RATE || max_fragment != MODEM_FRAG ||
	    operations != &v8_operations || operations->use_count == INT_MAX ||
	    modulation == V8_MODULATION_NONE)
		return NULL;

	lapm = modem_get_sreg(modem, SREG_EC) != 0;
	if (v8_datapump_uses_pcm(id)) {
		digital = modem_get_param(
			modem, MDMPRM_V90_DIGITAL_INDICATION) != 0;
		menu_result = v8_pcm_call_menu_init(
			&local_menu,
			digital ? V8_PCM_DIGITAL : V8_PCM_ANALOG,
			digital ? V8_PSTN_DIGITAL : 0, lapm);
	} else {
		menu_result = v8_call_menu_init(
			&local_menu, modulation, lapm);
	}
	if (menu_result != V8_MENU_OK)
		return NULL;
	datapump = calloc(1, sizeof(*datapump));
	if (datapump == NULL)
		return NULL;
	datapump->caller = caller != 0;
	if ((datapump->caller &&
	     v8_calling_init(
		     &datapump->handshake.calling, &local_menu) !=
		     V8_CALLING_OK) ||
	    (!datapump->caller &&
	     v8_answering_init(
		     &datapump->handshake.answering, &local_menu) !=
		     V8_ANSWERING_OK)) {
		free(datapump);
		return NULL;
	}
	datapump->requested_datapump = id;
	datapump->negotiated_modulation = modulation;
	datapump->dp = (struct dp){
		.id = DP_V8,
		.modem = modem,
		.status = DPSTAT_OK,
		.op = operations,
		.dp_data = datapump,
	};
	operations->use_count++;
	return &datapump->dp;
}

static int delete_v8(struct dp *dp)
{
	struct v8_datapump *datapump = v8_from_dp(dp);

	if (datapump == NULL || v8_operations.use_count <= 0)
		return -1;
	v8_operations.use_count--;
	datapump->dp.dp_data = NULL;
	free(datapump);
	return 0;
}

static bool selected_pcm_role(
	const struct v8_datapump *datapump,
	const struct v8_menu *joint_menu,
	enum modem_pcm_role *role)
{
	const struct v8_menu *call_menu;
	struct v8_pcm_roles roles;
	enum v8_pcm_role local_role;

	if (datapump->caller)
		call_menu = &datapump->handshake.calling.local_menu;
	else
		call_menu = &datapump->handshake.answering.call_menu;
	if (v8_pcm_select_roles(call_menu, joint_menu, &roles) != V8_MENU_OK)
		return false;
	local_role = datapump->caller ? roles.caller : roles.answerer;
	if (local_role == V8_PCM_ROLE_ANALOG)
		*role = MODEM_PCM_ROLE_ANALOG;
	else if (local_role == V8_PCM_ROLE_DIGITAL)
		*role = MODEM_PCM_ROLE_DIGITAL;
	else
		*role = MODEM_PCM_ROLE_NONE;
	return true;
}

static int request_handoff(struct v8_datapump *datapump)
{
	struct v8_menu menu;
	enum v8_modulation modulation;
	enum modem_pcm_role pcm_role = MODEM_PCM_ROLE_NONE;
	enum DP_ID handoff = datapump->requested_datapump;
	bool selected;

	if (datapump->caller) {
		selected = v8_calling_selection(
			&datapump->handshake.calling, &menu, &modulation) ==
			V8_CALLING_OK;
	} else {
		selected = v8_answering_selection(
			&datapump->handshake.answering, &menu, &modulation) ==
			V8_ANSWERING_OK;
	}
	if (!selected || modulation != datapump->negotiated_modulation ||
	    menu.modulations != datapump->negotiated_modulation)
		return DPSTAT_ERROR;
	if (v8_datapump_uses_pcm(datapump->requested_datapump)) {
		if (!selected_pcm_role(datapump, &menu, &pcm_role))
			return DPSTAT_ERROR;
		if (!menu.pcm_present)
			handoff = DP_V34;
		else if (pcm_role == MODEM_PCM_ROLE_NONE)
			return DPSTAT_ERROR;
	} else if (menu.pcm_present) {
		return DPSTAT_ERROR;
	}
	if (modem_set_param(
		    datapump->dp.modem, MDMPRM_DP_REQUESTED,
		    handoff) != 0)
		return DPSTAT_ERROR;
	datapump->dp.modem->dsp_info.pcm_role = pcm_role;
	datapump->dp.modem->cfg.ec = menu.lapm;
	datapump->dp.modem->dsp_info.qc_lapm = menu.lapm;
	datapump->handoff_requested = true;
	datapump->dp.status = DPSTAT_CHANGEDP;
	return DPSTAT_CHANGEDP;
}

static int process_v8(
	struct dp *dp, void *receive_buffer, void *transmit_buffer, int count)
{
	struct v8_datapump *datapump = v8_from_dp(dp);
	const int16_t *receive = receive_buffer;
	int16_t *transmit = transmit_buffer;
	bool complete;
	bool failed;

	if (datapump == NULL || receive == NULL || transmit == NULL ||
	    count <= 0 || count > MODEM_FRAG) {
		if (transmit != NULL && count > 0 && count <= MODEM_FRAG)
			memset(transmit, 0, (size_t)count * sizeof(*transmit));
		return DPSTAT_ERROR;
	}
	if (datapump->handoff_requested) {
		memset(transmit, 0, (size_t)count * sizeof(*transmit));
		return DPSTAT_CHANGEDP;
	}
	if (datapump->caller) {
		enum v8_calling_result result =
			v8_calling_process(
				&datapump->handshake.calling, receive, transmit,
				(size_t)count);

		failed = result != V8_CALLING_OK;
		complete = v8_calling_is_complete(
			&datapump->handshake.calling);
		if (result == V8_CALLING_ANSWER_TIMEOUT) {
			memset(transmit, 0, (size_t)count * sizeof(*transmit));
			return DPSTAT_NOANSWER;
		}
	} else {
		failed = v8_answering_process(
				 &datapump->handshake.answering, receive, transmit,
				 (size_t)count) != V8_ANSWERING_OK;
		complete = v8_answering_is_complete(
			&datapump->handshake.answering);
	}
	if (failed) {
		memset(transmit, 0, (size_t)count * sizeof(*transmit));
		return DPSTAT_ERROR;
	}
	return complete ? request_handoff(datapump) : DPSTAT_OK;
}

int v8_datapump_register(void)
{
	return modem_dp_register(DP_V8, &v8_operations);
}

int v8_datapump_unregister(void)
{
	return modem_dp_deregister(DP_V8, &v8_operations);
}

bool v8_datapump_is_busy(void)
{
	return v8_operations.use_count != 0;
}
