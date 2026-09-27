// SPDX-License-Identifier: BSD-3-Clause

#include "v8_datapump.h"

#include "datapump_media.h"
#include "datapump_timing.h"
#include "memory_region.h"
#include "modem.h"
#include "modem_param.h"
#include "v8_answering.h"
#include "v8_calling.h"
#include "v90_datapump.h"

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
	struct v8_pcm_handoff handoff;
	enum DP_ID requested_datapump;
	uint16_t offered_modulations;
	bool caller;
	bool handoff_requested;
	bool v34_fallback;
	bool restarted;
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

static const struct v8_datapump *v8_from_dp_const(const struct dp *dp)
{
	const struct v8_datapump *datapump;
	enum v8_modulation modulation;

	if (dp == NULL || dp->id != DP_V8 || dp->op != &v8_operations ||
	    dp->modem == NULL || dp->dp_data == NULL)
		return NULL;
	datapump = dp->dp_data;
	modulation = v8_datapump_modulation(
		datapump->requested_datapump);
	if (&datapump->dp != dp ||
	    modulation == V8_MODULATION_NONE ||
	    (modulation & datapump->offered_modulations) == 0 ||
	    datapump->handoff.tail_count >= MODEM_FRAG ||
	    (!datapump->handoff.quick_connect && datapump->handoff.tail_count != 0) ||
	    (datapump->requested_datapump == DP_V92 &&
	     !training_session_is_valid(&datapump->handoff.training)))
		return NULL;
	return datapump;
}

static struct v8_datapump *v8_from_dp(struct dp *dp)
{
	return (struct v8_datapump *)v8_from_dp_const(dp);
}

/* V.8 selects a family; rate exchange remains owned by that datapump.
 * Only advertise fallbacks with at least one rate inside the AT+MS range. */
static enum DP_ID fallback_datapump(
	const struct modem *modem, enum v8_modulation modulation)
{
	static const struct {
		enum DP_ID id;
		unsigned int first_rate, last_rate, step;
	} profiles[] = {
		{ DP_V32, 4800, 14400, 2400 },
		{ DP_V22BIS, 2400, 2400, 1 },
		{ DP_V22, 600, 1200, 600 },
		{ DP_V23, 1200, 1200, 1 },
		{ DP_V21, 300, 300, 1 },
	};

	for (size_t i = 0; i < sizeof(profiles) / sizeof(profiles[0]); ++i) {
		if (v8_datapump_modulation(profiles[i].id) != modulation ||
		    modem_dp_get_registered_ops(profiles[i].id) == NULL)
			continue;
		for (unsigned int rate = profiles[i].first_rate;
		     rate <= profiles[i].last_rate; rate += profiles[i].step)
			if (modem->min_rate <= rate && modem->max_rate >= rate)
				return profiles[i].id;
	}
	return 0;
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
	uint16_t offered = modulation;
	bool digital = false;
	bool lapm;
	bool restarting = modem != NULL && modem->dp != NULL &&
		modem->dp->id == DP_V92;

	if (modem == NULL || (caller != 0 && caller != 1) ||
	    sample_rate != MODEM_RATE || max_fragment != MODEM_FRAG ||
	    operations != &v8_operations || operations->use_count == INT_MAX ||
	    modulation == V8_MODULATION_NONE)
		return NULL;

	lapm = modem_get_sreg(modem, SREG_EC) != 0;
	if (MODEM_AUTOMODE(modem)) {
		/* Modulation bits are ordered from the highest family downward. */
		for (unsigned int bit = (unsigned int)modulation << 1;
		     bit <= V8_MODULATION_V21; bit <<= 1)
			if (fallback_datapump(modem, (enum v8_modulation)bit) != 0)
				offered |= bit;
	}
	menu_result = v8_call_menu_init(&local_menu, offered, lapm);
	if (menu_result != V8_MENU_OK)
		return NULL;
	if (v8_datapump_uses_pcm(id)) {
		digital = modem_get_param(
			modem, MDMPRM_V90_DIGITAL_INDICATION) != 0;
		local_menu.pcm_present = true;
		local_menu.pstn_access_present = true;
		local_menu.pcm_availability = digital ? V8_PCM_DIGITAL : V8_PCM_ANALOG;
		local_menu.pstn_access = digital ? V8_PSTN_DIGITAL : 0;
	}
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
	datapump->offered_modulations = offered;
	if (id == DP_V92) {
		if (restarting ?
		    !v90_datapump_v8_fallback_handoff(modem->dp, &datapump->handoff.training) :
		    training_session_init(&datapump->handoff.training,
			    datapump_carrier_wait_ticks(modem)) != TRAINING_SESSION_OK) {
			free(datapump);
			return NULL;
		}
		datapump->restarted = restarting;
		if (restarting && !caller)
			datapump->handshake.answering.quiet_samples = 0;
		if (!restarting && modem_get_param(modem, MDMPRM_V92_QUICK_CONNECT)) {
			struct v92_qc1_message local = {
				.sender = digital ? V90_DIGITAL_MODEM : V90_ANALOG_MODEM,
				.acknowledgment = !caller,
				.lapm = lapm,
			};

			/* Table 2 index 0 selects UQTS 61; Table 11 level 0 selects
			 * -9.5 dBm0 ANSpcm. These are fresh signal parameters, not
			 * saved channel training or permission for short Phase 2. */
			if (caller ? v8_calling_enable_quick_connect(
				    &datapump->handshake.calling, &local) != V8_CALLING_OK :
			    v8_answering_enable_quick_connect(
				    &datapump->handshake.answering, &local) != V8_ANSWERING_OK) {
				free(datapump);
				return NULL;
			}
		}
	}
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
	bool v34_fallback = false;
	bool lapm;
	bool qc = datapump->caller ?
		datapump->handshake.calling.state == V8_CALLING_QC_COMPLETE :
		datapump->handshake.answering.state == V8_ANSWERING_QC_COMPLETE;

	if (qc) {
		if (handoff != DP_V92 || datapump->restarted)
			return DPSTAT_ERROR;
		datapump->handoff.local = datapump->caller ?
			datapump->handshake.calling.qc_request :
			datapump->handshake.answering.qc_response;
		datapump->handoff.peer = datapump->caller ?
			datapump->handshake.calling.qc_response :
			datapump->handshake.answering.qc_request;
		datapump->handoff.quick_connect = true;
		pcm_role = datapump->handoff.local.sender == V90_DIGITAL_MODEM ?
			MODEM_PCM_ROLE_DIGITAL : MODEM_PCM_ROLE_ANALOG;
		lapm = datapump->handoff.local.lapm && datapump->handoff.peer.lapm;
		goto selected;
	}

	if (datapump->caller) {
		selected = v8_calling_selection(
			&datapump->handshake.calling, &menu, &modulation) ==
			V8_CALLING_OK;
	} else {
		selected = v8_answering_selection(
			&datapump->handshake.answering, &menu, &modulation) ==
			V8_ANSWERING_OK;
	}
	if (!selected || (modulation & datapump->offered_modulations) == 0 ||
	    (menu.modulations & ~datapump->offered_modulations) != 0)
		return DPSTAT_ERROR;
	if (modulation != v8_datapump_modulation(handoff)) {
		handoff = fallback_datapump(datapump->dp.modem, modulation);
		if (handoff == 0)
			return DPSTAT_ERROR;
	}
	if (v8_datapump_uses_pcm(handoff)) {
		if (!selected_pcm_role(datapump, &menu, &pcm_role))
			return DPSTAT_ERROR;
		if (!menu.pcm_present) {
			handoff = DP_V34;
			v34_fallback = true;
		} else if (pcm_role == MODEM_PCM_ROLE_NONE) {
			return DPSTAT_ERROR;
		}
	} else if (menu.pcm_present) {
		return DPSTAT_ERROR;
	}
	lapm = menu.lapm;
selected:
	if (modem_set_param(
		    datapump->dp.modem, MDMPRM_DP_REQUESTED,
		    handoff) != 0)
		return DPSTAT_ERROR;
	datapump->dp.modem->dsp_info.pcm_role = pcm_role;
	/* V.8 prot0 can select LAPM without ODP/ADP, but omitting prot0 does
	 * not decline later V.42 negotiation.  Retain the local AT\N choice so
	 * the ordinary detector can resolve the peer's selection. */
	if (lapm)
		datapump->dp.modem->cfg.ec = 1;
	datapump->dp.modem->dsp_info.v8_lapm = lapm;
	datapump->dp.modem->dsp_info.startup = qc ? MODEM_STARTUP_UNKNOWN :
		datapump->restarted ? MODEM_STARTUP_V8_FALLBACK : MODEM_STARTUP_V8;
	datapump->v34_fallback = v34_fallback;
	datapump->handoff_requested = true;
	datapump->dp.status = DPSTAT_CHANGEDP;
	return DPSTAT_CHANGEDP;
}

static int process_v8_samples(
	struct dp *dp, const void *receive_buffer, void *transmit_buffer, int count)
{
	struct v8_datapump *datapump = v8_from_dp(dp);
	const int16_t *receive = receive_buffer;
	int16_t *transmit = transmit_buffer;
	bool complete;
	bool failed;

	if (!datapump_linear_fragment_is_ready(datapump, receive, transmit, count))
		return DPSTAT_ERROR;
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

static int process_v8(struct dp *dp, void *receive_buffer, void *transmit_buffer, int count)
{
	struct v8_datapump *datapump = v8_from_dp(dp);
	const int16_t *receive = receive_buffer;
	int16_t *transmit = transmit_buffer;

	if (datapump == NULL || datapump->requested_datapump != DP_V92 ||
	    receive == NULL || transmit == NULL || count <= 0 || count > MODEM_FRAG ||
	    datapump->handoff_requested)
		return process_v8_samples(dp, receive_buffer, transmit_buffer, count);
	if (training_session_advance(&datapump->handoff.training, (unsigned)count) != TRAINING_SESSION_OK) {
		memset(transmit, 0, (size_t)count * sizeof(*transmit));
		return DPSTAT_ERROR;
	}
	for (int index = 0; index < count; ++index) {
		int result = process_v8_samples(dp, receive + index, transmit + index, 1);

		if (result == DPSTAT_OK)
			continue;
		if (result == DPSTAT_CHANGEDP && datapump->handoff.quick_connect) {
			datapump->handoff.tail_count = (unsigned)(count - index - 1);
			memcpy(datapump->handoff.receive_tail, receive + index + 1,
			       datapump->handoff.tail_count * sizeof(*receive));
		}
		memset(transmit + index + 1, 0, (size_t)(count - index - 1) * sizeof(*transmit));
		return result;
	}
	return DPSTAT_OK;
}

bool v8_datapump_pcm_handoff(const struct dp *dp, struct v8_pcm_handoff *handoff)
{
	const struct v8_datapump *datapump = v8_from_dp_const(dp);

	if (datapump == NULL || handoff == NULL || !datapump->handoff_requested ||
	    datapump->requested_datapump != DP_V92 || datapump->v34_fallback ||
	    training_session_has_failed(&datapump->handoff.training) ||
	    memory_regions_overlap(datapump, sizeof(*datapump), handoff, sizeof(*handoff)))
		return false;
	*handoff = datapump->handoff;
	return true;
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

bool v8_datapump_selected_v34_fallback(const struct dp *dp)
{
	const struct v8_datapump *datapump = v8_from_dp_const(dp);

	return datapump != NULL && datapump->handoff_requested &&
	       datapump->v34_fallback;
}
