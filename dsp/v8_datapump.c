// SPDX-License-Identifier: BSD-3-Clause

#include "v8_datapump.h"

#include "datapump.h"
#include "ds0/ds0_datapump.h"
#include "datapump_media.h"
#include "datapump_timing.h"
#include "memory_region.h"
#include "modem.h"
#include "single_tone.h"
#include "v22_datapump.h"
#include "v8_answering.h"
#include "v8_calling.h"
#include "v34_info0.h"
#include "v34_phase2_info_audio.h"
#include "v90_datapump.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
	LEGACY_TONE_COUNT = 5,
	LEGACY_ANSWER_SAMPLES = 5 * MODEM_RATE,
	LEGACY_GAP_SAMPLES = MODEM_RATE * 75 / 1000,
};

static const struct {
	unsigned frequency;
	enum DP_ID id;
} legacy_tones[LEGACY_TONE_COUNT] = {
	{ 2100, DP_V32BIS }, { 2250, DP_V22BIS }, { 2225, DP_B103 },
	{ 1650, DP_V21 }, { 1300, DP_V23 },
};

static const struct {
	enum DP_ID id;
	unsigned samples;
	bool rate600;
} legacy_stages[] = {
	{ DP_V32BIS, 3 * MODEM_RATE, false },
	{ DP_V22BIS, 2 * MODEM_RATE, false },
	{ DP_V22, 3 * MODEM_RATE, true },
	{ DP_B103, MODEM_RATE * 3 / 5, false },
	{ DP_B212, 2 * MODEM_RATE, false },
	{ DP_V21, 2 * MODEM_RATE, false },
	{ DP_V23, 2 * MODEM_RATE, false },
};

struct v8_datapump {
	struct dp dp;
	union {
		struct v8_calling calling;
		struct v8_answering answering;
	} handshake;
	struct v8_pcm_handoff handoff;
	struct v34_phase2_info_audio_receiver info0_receiver;
	struct v34_info0 peer_info0;
	uint8_t info0_repetitions;
	struct single_tone_detector legacy_carrier;
	struct single_tone_detector answer_tones[LEGACY_TONE_COUNT];
	struct dp *legacy;
	struct ds0_probe *ds0_probe;
	uint32_t elapsed_samples;
	uint32_t legacy_samples;
	uint32_t answer_tone_samples;
	uint32_t high_tone_samples;
	uint32_t high_tone_crossings;
	int16_t previous_receive;
	unsigned legacy_stage;
	bool legacy_answer;
	bool legacy_selected;
	unsigned int legacy_carrier_samples;
	enum DP_ID requested_datapump;
	uint16_t offered_modulations;
	bool caller;
	bool handoff_requested;
	bool v34_fallback;
	bool restarted;
	bool legacy_v32;
};

static struct dp *create_v8(
	struct modem *modem, enum DP_ID id, int caller, int sample_rate,
	int max_fragment, const struct dp_operations *operations);
static int delete_v8(struct dp *dp);
static int process_v8(
	struct dp *dp, void *receive, void *transmit, int count);
static const char *progress_v8(const struct dp *dp);
static int process_v8_g711(struct dp *, enum g711_law, const uint8_t *, void *,
	const bool *, uint8_t *, void *, int, bool *);

static const struct dp_operations v8_operations = {
	.name = "V.8/legacy negotiation",
	.create = create_v8,
	.delete = delete_v8,
	.process = process_v8,
	.process_g711 = process_v8_g711,
	.progress = progress_v8,
};

static const char *progress_v8(const struct dp *dp)
{
	const struct v8_datapump *datapump = dp->dp_data;
	if (datapump == NULL)
		return "V.8/initializing";
	if (datapump->legacy != NULL)
		return datapump->legacy->op->progress != NULL ?
			datapump->legacy->op->progress(datapump->legacy) : "legacy/training";
	if (datapump->legacy_answer)
		return "legacy/answer-probes";
	if (datapump->handoff_requested)
		return "V.8/modulation-selected";
	if (datapump->caller) {
		switch (datapump->handshake.calling.state) {
		case V8_CALLING_WAIT_ANSWER_SIGNAL: return "V.8/wait-ANSam";
		case V8_CALLING_EXCHANGE_MENUS: return "V.8/CM-wait-JM";
		case V8_CALLING_SEND_CJ: return "V.8/send-CJ";
		case V8_CALLING_SEND_QC1:
		case V8_CALLING_QC_TX_TAIL:
		case V8_CALLING_QC_COMPLETE: return "V.92/QC1";
		case V8_CALLING_COMPLETE: return "V.8/complete";
		case V8_CALLING_FAILED:
			return datapump->handshake.calling.terminal_result == V8_CALLING_ANSWER_TIMEOUT ?
				"V.8/wait-ANSam" : "V.8/CM-wait-JM";
		}
	} else {
		switch (datapump->handshake.answering.state) {
		case V8_ANSWERING_WAIT_CALL_MENU: return "V.8/ANSam-wait-CM";
		case V8_ANSWERING_SEND_JOINT_MENU: return "V.8/JM-wait-CJ";
		case V8_ANSWERING_SEND_QCA1:
		case V8_ANSWERING_QC_COMPLETE: return "V.92/QCA1";
		case V8_ANSWERING_COMPLETE: return "V.8/complete";
		case V8_ANSWERING_FAILED:
			return datapump->handshake.answering.terminal_result == V8_ANSWERING_CJ_TIMEOUT ?
				"V.8/JM-wait-CJ" : "V.8/ANSam-wait-CM";
		}
	}
	return "V.8/unknown";
}

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
	    ((datapump->requested_datapump == DP_V92 || datapump->requested_datapump == DP_V91) &&
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
		{ DP_V32BIS, 4800, 14400, 2400 },
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
		     rate <= profiles[i].last_rate; rate += profiles[i].step) {
			if (modem->min_rate > rate || modem->max_rate < rate)
				continue;
			if (profiles[i].id == DP_V32BIS && modem->max_rate <= 9600 &&
			    (modem->min_rate > 7200 || modem->max_rate < 7200))
				return DP_V32;
			return profiles[i].id;
		}
	}
	return 0;
}

static struct dp *create_v8(
	struct modem *modem, enum DP_ID id, int caller, int sample_rate,
	int max_fragment, const struct dp_operations *operations)
{
	struct v8_datapump *datapump;
	struct v8_menu local_menu;
	enum v8_modulation modulation =
		v8_datapump_modulation(id);
	enum v8_menu_result menu_result;
	uint16_t offered = modulation;
	unsigned role_policy = MODEM_PCM_POLICY_ANALOG;
	bool digital = false;
	bool lapm;
	bool restarting = modem != NULL && modem->dp != NULL &&
		modem->dp->id == DP_V92;

	if (modem == NULL || (caller != 0 && caller != 1) ||
	    sample_rate != MODEM_RATE || max_fragment != MODEM_FRAG ||
	    operations != &v8_operations ||
	    modulation == V8_MODULATION_NONE ||
	    (id == DP_V91 && !modem_get_sreg_flag(modem, SREG_V91_ENABLED)))
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
		role_policy = modem_get_sreg(modem, SREG_V90_DIGITAL_INDICATION);
		if (role_policy > MODEM_PCM_POLICY_AUTO)
			return NULL;
		digital = role_policy == MODEM_PCM_POLICY_DIGITAL;
		local_menu.pcm_present = true;
		local_menu.pstn_access_present = true;
		local_menu.pcm_availability = role_policy == MODEM_PCM_POLICY_AUTO ?
			V8_PCM_ANALOG | V8_PCM_DIGITAL : digital ? V8_PCM_DIGITAL : V8_PCM_ANALOG;
		local_menu.pstn_access = role_policy != MODEM_PCM_POLICY_ANALOG ? V8_PSTN_DIGITAL : 0;
		/* Nailed V.90/V.92 retain their requested protocol. Automatic
		 * digital peers may select the standard bidirectional PCM mode. */
		if (id == DP_V91) {
			local_menu.pcm_availability = V8_PCM_V91;
			local_menu.pstn_access = V8_PSTN_DIGITAL;
		} else if (id == DP_V92 && MODEM_AUTOMODE(modem) &&
			   modem_get_sreg_flag(modem, SREG_V91_ENABLED) &&
			   role_policy != MODEM_PCM_POLICY_ANALOG && modem->max_rate >= 28000) {
			local_menu.pcm_availability |= V8_PCM_V91;
		}
	}
	datapump = calloc(1, sizeof(*datapump));
	if (datapump == NULL)
		return NULL;
	datapump->caller = caller != 0;
	if (caller && (MODEM_AUTOMODE(modem) || id == DP_V34) &&
	    modulation == V8_MODULATION_V34_DUPLEX &&
	    v34_phase2_info_audio_receiver_init(&datapump->info0_receiver,
		    V34_CALLING_STATION, V34_INFO0_PAYLOAD_BITS) !=
		    V34_PHASE2_INFO_AUDIO_OK) {
		free(datapump);
		return NULL;
	}
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
	if (caller && MODEM_AUTOMODE(modem)) {
		for (unsigned i = 0; i < LEGACY_TONE_COUNT; ++i) {
			if (single_tone_detector_init(&datapump->answer_tones[i],
				    legacy_tones[i].frequency) != SINGLE_TONE_OK) {
				free(datapump);
				return NULL;
			}
		}
	}
	if (!caller && MODEM_AUTOMODE(modem) &&
	    (offered & V8_MODULATION_V32) != 0 &&
	    fallback_datapump(modem, V8_MODULATION_V32) != 0 &&
	    single_tone_detector_init(&datapump->legacy_carrier, 1800) != SINGLE_TONE_OK) {
		free(datapump);
		return NULL;
	}
	if (id == DP_V92 || id == DP_V91) {
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
		/* QC1 commits to a PCM role before menus; automatic roles need CM/JM. */
		if (id == DP_V92 && !restarting && role_policy != MODEM_PCM_POLICY_AUTO &&
		    modem_get_sreg_flag(modem, SREG_V92_QUICK_CONNECT)) {
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
	bool allow56 = modem->min_rate <= 56000 && modem->max_rate >= 56000;
	bool allow64 = modem->min_rate <= 64000 && modem->max_rate >= 64000;
	if (MODEM_AUTOMODE(modem) && (allow56 || allow64)) {
		datapump->ds0_probe = malloc(sizeof(*datapump->ds0_probe));
		if (datapump->ds0_probe == NULL ||
		    ds0_probe_init(datapump->ds0_probe, allow56, allow64) != 0) {
			free(datapump->ds0_probe);
			free(datapump);
			return NULL;
		}
	}
	datapump->dp = (struct dp){
		.id = DP_V8,
		.modem = modem,
		.status = DPSTAT_OK,
		.op = operations,
		.dp_data = datapump,
	};
	return &datapump->dp;
}

static int delete_v8(struct dp *dp)
{
	struct v8_datapump *datapump = v8_from_dp(dp);

	if (datapump == NULL)
		return -1;
	if (datapump->legacy != NULL)
		datapump->legacy->op->delete(datapump->legacy);
	free(datapump->ds0_probe);
	datapump->dp.dp_data = NULL;
	free(datapump);
	return 0;
}

static bool selected_pcm_role(
	const struct v8_datapump *datapump,
	const struct v8_menu *joint_menu,
	enum modem_pcm_role *role, bool *duplex)
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
	*duplex = roles.caller == V8_PCM_ROLE_DIGITAL && roles.answerer == V8_PCM_ROLE_DIGITAL;
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
	bool duplex = false;
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
	if (!selected ||
	    (menu.modulations & ~datapump->offered_modulations) != 0)
		return DPSTAT_ERROR;
	if (v8_datapump_uses_pcm(handoff) &&
	    !selected_pcm_role(datapump, &menu, &pcm_role, &duplex))
		return DPSTAT_ERROR;
	if (duplex) {
		handoff = DP_V91;
	} else {
		if ((modulation & datapump->offered_modulations) == 0)
			return DPSTAT_ERROR;
		if (modulation != v8_datapump_modulation(handoff)) {
			handoff = fallback_datapump(datapump->dp.modem, modulation);
			if (handoff == 0)
				return DPSTAT_ERROR;
		}
		if (v8_datapump_uses_pcm(handoff)) {
			if (!menu.pcm_present) {
				handoff = DP_V34;
				v34_fallback = true;
			} else if (pcm_role == MODEM_PCM_ROLE_NONE) {
				return DPSTAT_ERROR;
			}
		} else if (menu.pcm_present) {
			return DPSTAT_ERROR;
		}
	}
	lapm = menu.lapm;
selected:
	datapump->dp.modem->dp_requested = handoff;
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
		if (failed)
			modem_note_disconnect(dp->modem, true, v8_calling_result_name(result));
		complete = v8_calling_is_complete(
			&datapump->handshake.calling);
		if (result == V8_CALLING_ANSWER_TIMEOUT) {
			memset(transmit, 0, (size_t)count * sizeof(*transmit));
			return DPSTAT_NOANSWER;
		}
	} else {
		enum v8_answering_result result = v8_answering_process(
				 &datapump->handshake.answering, receive, transmit,
				 (size_t)count);
		failed = result != V8_ANSWERING_OK;
		if (failed)
			modem_note_disconnect(dp->modem, true, v8_answering_result_name(result));
		complete = v8_answering_is_complete(
			&datapump->handshake.answering);
	}
	if (failed) {
		memset(transmit, 0, (size_t)count * sizeof(*transmit));
		return DPSTAT_ERROR;
	}
	return complete ? request_handoff(datapump) : DPSTAT_OK;
}

static int process_handshake(struct dp *dp, void *receive_buffer, void *transmit_buffer, int count)
{
	struct v8_datapump *datapump = v8_from_dp(dp);
	const int16_t *receive = receive_buffer;
	int16_t *transmit = transmit_buffer;

	if (datapump == NULL || (datapump->requested_datapump != DP_V92 && datapump->requested_datapump != DP_V91) ||
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

/* Legacy probes use the same family datapumps as fixed-mode calls. Transfer
 * the trained instance to the host instead of repeating carrier acquisition. */
static enum DP_ID legacy_id(const struct v8_datapump *datapump, enum DP_ID id)
{
	const struct modem *modem = datapump->dp.modem;
	enum v8_modulation family = v8_datapump_modulation(id);

	if (!MODEM_AUTOMODE(modem))
		return 0;
	if (id == DP_B212)
		return (datapump->offered_modulations & V8_MODULATION_V22) != 0 &&
			modem->min_rate <= 1200 && modem->max_rate >= 1200 ? id : 0;
	if (id == DP_B103)
		return (datapump->offered_modulations & V8_MODULATION_V21) != 0 &&
			modem->min_rate <= 300 && modem->max_rate >= 300 ? id : 0;
	if ((datapump->offered_modulations & family) == 0)
		return 0;
	if (family == v8_datapump_modulation(datapump->requested_datapump))
		return datapump->requested_datapump;
	return fallback_datapump(modem, family);
}

static bool start_legacy(struct v8_datapump *datapump, enum DP_ID id, bool rate600)
{
	struct modem *modem = datapump->dp.modem;
	const struct dp_operations *ops = modem_dp_get_registered_ops(id);
	unsigned min_rate = modem->min_rate, max_rate = modem->max_rate;
	struct dp *next;

	if (ops == NULL || (rate600 && (min_rate > 600 || max_rate < 600)))
		return false;
	if (rate600)
		modem->min_rate = modem->max_rate = 600;
	next = ops->create(modem, id, datapump->caller, MODEM_RATE, MODEM_FRAG, ops);
	modem->min_rate = min_rate;
	modem->max_rate = max_rate;
	if (next == NULL)
		return false;
	if (datapump->legacy != NULL)
		datapump->legacy->op->delete(datapump->legacy);
	datapump->legacy = next;
	datapump->legacy_samples = 0;
	return true;
}

static int select_legacy(struct v8_datapump *datapump)
{
	struct modem *modem = datapump->dp.modem;

	modem->dp_requested = datapump->legacy->id;
	modem->dsp_info.startup = MODEM_STARTUP_UNKNOWN;
	modem->dsp_info.v8_lapm = false;
	modem->dsp_info.pcm_role = MODEM_PCM_ROLE_NONE;
	datapump->legacy_selected = true;
	datapump->handoff_requested = true;
	datapump->dp.status = DPSTAT_CHANGEDP;
	return DPSTAT_CHANGEDP;
}

static int next_answer_probe(struct v8_datapump *datapump)
{
	while (datapump->legacy_stage < sizeof(legacy_stages) / sizeof(legacy_stages[0])) {
		unsigned stage = datapump->legacy_stage++;
		enum DP_ID id = legacy_id(datapump, legacy_stages[stage].id);
		bool rate600 = legacy_stages[stage].rate600;

		if (id == 0 || (rate600 && (datapump->dp.modem->min_rate > 600 ||
		    datapump->dp.modem->max_rate < 600)))
			continue;
		if (start_legacy(datapump, rate600 ? DP_V22 : id, rate600))
			return DPSTAT_OK;
		return DPSTAT_ERROR;
	}
	return DPSTAT_NOANSWER;
}

/* A peer may already be in Phase 2 while we are still waiting for ANSam
 * or JM. Require two matching, CRC-valid INFO0 records before recovering;
 * never supersede a completed menu or confirmed legacy training. */
static int observe_v34_info0(
	struct v8_datapump *datapump, const int16_t *receive, int count)
{
	struct modem *modem = datapump->dp.modem;

	if (!datapump->caller ||
	    (!MODEM_AUTOMODE(modem) && datapump->requested_datapump != DP_V34) ||
	    v8_datapump_modulation(datapump->requested_datapump) !=
		    V8_MODULATION_V34_DUPLEX ||
	    (datapump->handshake.calling.state != V8_CALLING_WAIT_ANSWER_SIGNAL &&
	     datapump->handshake.calling.state != V8_CALLING_EXCHANGE_MENUS) ||
	    (datapump->legacy != NULL &&
	     (v8_datapump_modulation(datapump->legacy->id) != V8_MODULATION_V32 ||
	      v32_datapump_has_peer(datapump->legacy))))
		return DPSTAT_OK;

	for (int i = 0; i < count; ++i) {
		struct v34_phase2_info_event event;
		struct v34_info0 peer;

		if (v34_phase2_info_audio_receiver_sample(
			    &datapump->info0_receiver, receive[i], &event) !=
			    V34_PHASE2_INFO_AUDIO_OK)
			return DPSTAT_ERROR;
		if (event.type == V34_PHASE2_INFO_FRAME_REJECTED) {
			datapump->info0_repetitions = 0;
			continue;
		}
		if (event.type != V34_PHASE2_INFO_FRAME_RECEIVED)
			continue;
		if (v34_info0_decode(event.payload, sizeof(event.payload),
			    event.payload_bit_count, &peer) != V34_INFO0_OK) {
			datapump->info0_repetitions = 0;
			continue;
		}
		if (datapump->info0_repetitions != 0 &&
		    v34_info0_equal(&datapump->peer_info0, &peer)) {
			++datapump->info0_repetitions;
		} else {
			datapump->peer_info0 = peer;
			datapump->info0_repetitions = 1;
		}
		if (datapump->info0_repetitions < 2)
			continue;

		modem->dp_requested = DP_V34;
		modem->dsp_info.startup = MODEM_STARTUP_UNKNOWN;
		modem->dsp_info.v8_lapm = false;
		modem->dsp_info.pcm_role = MODEM_PCM_ROLE_NONE;
		datapump->v34_fallback = true;
		datapump->handoff_requested = true;
		datapump->dp.status = DPSTAT_CHANGEDP;
		return DPSTAT_CHANGEDP;
	}
	return DPSTAT_OK;
}

static int observe_legacy_answer(struct v8_datapump *datapump, const int16_t *receive, int count)
{
	bool exchanging = datapump->legacy == NULL && datapump->caller &&
		datapump->handshake.calling.state == V8_CALLING_EXCHANGE_MENUS;
	bool settling = false;

	if (!datapump->caller || !MODEM_AUTOMODE(datapump->dp.modem))
		return DPSTAT_OK;
	if (datapump->legacy != NULL) {
		enum v8_modulation family = v8_datapump_modulation(datapump->legacy->id);

		/* ANS may precede another family's mark. A V.22 mark also
		 * need not lead to an exchange: a scanning peer can move on.
		 * Keep confirmed training, and give V.22 time to obtain S1 or
		 * scrambled marks before listening for a different family. */
		if (family == V8_MODULATION_V32) {
			if (v32_datapump_has_peer(datapump->legacy))
				return DPSTAT_OK;
		} else if (family != V8_MODULATION_V22 ||
			   v22_datapump_has_peer(datapump->legacy)) {
			return DPSTAT_OK;
		}
		settling = family == V8_MODULATION_V22 &&
			datapump->legacy_samples < 3 * MODEM_RATE;
	} else if (datapump->handshake.calling.state != V8_CALLING_WAIT_ANSWER_SIGNAL &&
		   !exchanging) {
		return DPSTAT_OK;
	}
	/* During active CM, admit only distinct 2225/2250-Hz marks:
	 * 2100 Hz remains ANSam and 1650 Hz can be the V.8 joint menu.
	 * A legacy child stops CM even though the stored V.8 state remains. */
	for (int sample = 0; sample < count; ++sample) {
		for (unsigned tone = exchanging ? 1 : 0;
		     tone < (exchanging ? 3 : LEGACY_TONE_COUNT); ++tone) {
			struct single_tone_event event;
			if (single_tone_detector_sample(&datapump->answer_tones[tone],
				    receive[sample], &event) != SINGLE_TONE_OK)
				return DPSTAT_ERROR;
		}
		/* Bell 103's 2225-Hz answer and the V.22 2250-Hz mark fit
		 * the same short tone window. Count cycles over 160 ms before
		 * choosing, as the fixed Bell caller does in its silent guard. */
		bool high = datapump->answer_tones[1].tone_present ||
			datapump->answer_tones[2].tone_present;
		if (!high) {
			datapump->high_tone_samples = 0;
			datapump->high_tone_crossings = 0;
		} else {
			datapump->high_tone_samples++;
			if (datapump->previous_receive <= 0 && receive[sample] > 0)
				datapump->high_tone_crossings++;
		}
		datapump->previous_receive = receive[sample];
		/* ANSam stops updating when CM starts. Its retained detector
		 * state must not accrue a false legacy V.32 handoff interval. */
		if (!exchanging)
			datapump->answer_tone_samples = datapump->answer_tones[0].tone_present ?
				datapump->answer_tone_samples + 1 : 0;
	}
	/* Clock the detectors during the grace period so a retained mark
	 * cannot cause a stale family selection when the period ends. */
	if (settling)
		return DPSTAT_OK;
	unsigned tone = LEGACY_TONE_COUNT;
	if (!exchanging && datapump->answer_tones[3].tone_present)
		tone = 3;
	else if (!exchanging && datapump->answer_tones[4].tone_present)
		tone = 4;
	else if (datapump->high_tone_samples >= MODEM_RATE * 160 / 1000) {
		uint64_t cycles = (uint64_t)datapump->high_tone_crossings * MODEM_RATE;
		tone = cycles < (uint64_t)datapump->high_tone_samples * 2238 ? 2 : 1;
	} else if (!exchanging && datapump->answer_tone_samples >= 2 * MODEM_RATE)
		tone = 0;
	if (tone != LEGACY_TONE_COUNT) {
		enum DP_ID id = legacy_id(datapump, legacy_tones[tone].id);
		if (tone == 2 && legacy_id(datapump, DP_B212) != 0)
			id = DP_B212;
		if (id != 0 && (datapump->legacy == NULL ||
		    (tone != 0 && v8_datapump_modulation(datapump->legacy->id) !=
			v8_datapump_modulation(id)))) {
			if (!start_legacy(datapump, id, false))
				return DPSTAT_ERROR;
		}
	}
	return DPSTAT_OK;
}

static int process_legacy(struct v8_datapump *datapump, const int16_t *receive, void *transmit, int count)
{
	int result = datapump->legacy->op->process(datapump->legacy, (void *)receive, transmit, count);

	datapump->legacy_samples += (unsigned)count;
	if (result == DPSTAT_CONNECT)
		return select_legacy(datapump);
	if (datapump->caller) {
		if (result == DPSTAT_OK && datapump->legacy->id == DP_B212 &&
		    datapump->legacy_samples >= 3 * MODEM_RATE &&
		    !v22_datapump_has_peer(datapump->legacy) &&
		    legacy_id(datapump, DP_B103) != 0)
			return start_legacy(datapump, DP_B103, false) ? DPSTAT_OK : DPSTAT_ERROR;
		if (result == DPSTAT_OK && datapump->legacy->id == DP_V22BIS &&
		    v22_datapump_can_retry_600(datapump->legacy) &&
		    datapump->dp.modem->min_rate <= 600 &&
		    datapump->dp.modem->max_rate >= 600)
			return start_legacy(datapump, DP_V22, true) ? DPSTAT_OK : DPSTAT_ERROR;
		return result;
	}
	unsigned limit = legacy_stages[datapump->legacy_stage - 1].samples;
	if (result == DPSTAT_OK && (datapump->legacy_samples < limit ||
	    v22_datapump_has_peer(datapump->legacy) ||
	    (v8_datapump_modulation(datapump->legacy->id) == V8_MODULATION_V32 &&
	     v32_datapump_has_peer(datapump->legacy))))
		return DPSTAT_OK;
	if (result != DPSTAT_OK && result != DPSTAT_ERROR)
		return result;
	return next_answer_probe(datapump);
}

struct dp *v8_datapump_take_legacy(struct dp *dp)
{
	struct v8_datapump *datapump = v8_from_dp(dp);
	struct dp *legacy;

	if (datapump == NULL || !datapump->legacy_selected)
		return NULL;
	legacy = datapump->legacy;
	datapump->legacy = NULL;
	return legacy;
}

bool v8_datapump_answer_tone_complete(const struct dp *dp)
{
	const struct v8_datapump *datapump = v8_from_dp_const(dp);

	return datapump != NULL && datapump->legacy_answer;
}

static int process_v8(struct dp *dp, void *receive_buffer, void *transmit_buffer, int count)
{
	struct v8_datapump *datapump = v8_from_dp(dp);
	const int16_t *receive = receive_buffer;
	bool listening;
	enum DP_ID handoff;
	int result;

	if (!datapump_linear_fragment_is_ready(datapump, receive, transmit_buffer, count))
		return DPSTAT_ERROR;
	if (datapump->handoff_requested) {
		memset(transmit_buffer, 0, (size_t)count * sizeof(*receive));
		return DPSTAT_CHANGEDP;
	}
	{
		uint32_t limit = datapump_carrier_wait_ticks(dp->modem);
		if (datapump->elapsed_samples >= limit ||
		    (unsigned)count > limit - datapump->elapsed_samples) {
			memset(transmit_buffer, 0, (size_t)count * sizeof(*receive));
			return DPSTAT_NOANSWER;
		}
		datapump->elapsed_samples += (unsigned)count;
	}
	result = observe_v34_info0(datapump, receive, count);
	if (result != DPSTAT_OK) {
		memset(transmit_buffer, 0, (size_t)count * sizeof(*receive));
		return result;
	}
	if (observe_legacy_answer(datapump, receive, count) != DPSTAT_OK)
		return DPSTAT_ERROR;
	if (datapump->legacy != NULL)
		return process_legacy(datapump, receive, transmit_buffer, count);
	if (datapump->legacy_answer) {
		memset(transmit_buffer, 0, (size_t)count * sizeof(*receive));
		datapump->legacy_samples += (unsigned)count;
		return datapump->legacy_samples < LEGACY_GAP_SAMPLES ? DPSTAT_OK :
			next_answer_probe(datapump);
	}
	listening = datapump->legacy_carrier.profile.frequency_hz != 0 &&
		datapump->handshake.answering.state == V8_ANSWERING_WAIT_CALL_MENU;
	/* A legacy V.32 caller answers ANSam with sustained 1800-Hz carrier.
	 * Recorded answerers leave ANSam early for this response, then use the
	 * ordinary 75-ms silence and AC exchange. Confirm half a second, not a
	 * short carrier fragment. Consume input before generating output so an
	 * in-place fragment preserves its complete carrier history too. */
	for (int i = 0; listening && i < count; ++i) {
		struct single_tone_event event;

		if (single_tone_detector_sample(&datapump->legacy_carrier,
			    receive[i], &event) != SINGLE_TONE_OK) {
			memset(transmit_buffer, 0, (size_t)count * sizeof(*receive));
			return DPSTAT_ERROR;
		}
		if (!datapump->legacy_carrier.tone_present ||
		    event.kind == SINGLE_TONE_PHASE_REVERSED)
			datapump->legacy_carrier_samples = 0;
		else if (datapump->legacy_carrier_samples < MODEM_RATE / 2)
			datapump->legacy_carrier_samples++;
	}
	if (!datapump->caller && MODEM_AUTOMODE(dp->modem) &&
	    datapump->handshake.answering.state == V8_ANSWERING_WAIT_CALL_MENU &&
	    datapump->handshake.answering.state_samples >=
		V8_ANSWERING_QUIET_SAMPLES + LEGACY_ANSWER_SAMPLES) {
		datapump->legacy_answer = true;
		datapump->legacy_samples = 0;
		memset(transmit_buffer, 0, (size_t)count * sizeof(*receive));
		return DPSTAT_OK;
	}
	result = process_handshake(dp, receive_buffer, transmit_buffer, count);
	if (result != DPSTAT_OK || !listening ||
	    datapump->handshake.answering.state != V8_ANSWERING_WAIT_CALL_MENU ||
	    datapump->legacy_carrier_samples < MODEM_RATE / 2)
		return result;
	handoff = v8_datapump_modulation(datapump->requested_datapump) == V8_MODULATION_V32 ?
		datapump->requested_datapump : fallback_datapump(dp->modem, V8_MODULATION_V32);
	if (handoff == 0)
		return DPSTAT_ERROR;
	dp->modem->dp_requested = handoff;
	/* No CM/JM was exchanged: leave error-control detection enabled and do
	 * not report V.8 negotiation or an agreed PCM role. */
	dp->modem->dsp_info.startup = MODEM_STARTUP_UNKNOWN;
	dp->modem->dsp_info.v8_lapm = false;
	dp->modem->dsp_info.pcm_role = MODEM_PCM_ROLE_NONE;
	datapump->legacy_v32 = true;
	datapump->handoff_requested = true;
	dp->status = DPSTAT_CHANGEDP;
	return DPSTAT_CHANGEDP;
}

bool v8_datapump_v32_handoff(const struct dp *dp, struct single_tone_detector *carrier)
{
	const struct v8_datapump *datapump = v8_from_dp_const(dp);

	if (datapump == NULL || carrier == NULL || (!datapump->legacy_v32 &&
	     !(datapump->caller && datapump->answer_tone_samples >= 2 * MODEM_RATE)) ||
	    (!datapump->handoff_requested && !datapump->caller) ||
	    memory_regions_overlap(datapump, sizeof(*datapump), carrier, sizeof(*carrier)))
		return false;
	*carrier = datapump->caller ? datapump->answer_tones[0] : datapump->legacy_carrier;
	return true;
}

bool v8_datapump_pcm_handoff(const struct dp *dp, struct v8_pcm_handoff *handoff)
{
	const struct v8_datapump *datapump = v8_from_dp_const(dp);

	if (datapump == NULL || handoff == NULL || !datapump->handoff_requested ||
	    (datapump->requested_datapump != DP_V92 && datapump->requested_datapump != DP_V91) || datapump->v34_fallback || datapump->legacy_v32 || datapump->legacy_selected ||
	    training_session_has_failed(&datapump->handoff.training) ||
	    memory_regions_overlap(datapump, sizeof(*datapump), handoff, sizeof(*handoff)))
		return false;
	*handoff = datapump->handoff;
	return true;
}

const struct dp_operations *v8_datapump_operations(enum DP_ID id)
{
	return id == DP_V8 ? &v8_operations : NULL;
}

bool v8_datapump_selected_v34_fallback(const struct dp *dp)
{
	const struct v8_datapump *datapump = v8_from_dp_const(dp);

	return datapump != NULL && datapump->handoff_requested &&
	       datapump->v34_fallback;
}

static int process_v8_g711(struct dp *dp, enum g711_law law, const uint8_t *codewords,
	void *receive, const bool *available, uint8_t *output, void *transmit, int count, bool *exact)
{
	struct v8_datapump *p = v8_from_dp(dp);
	if (exact != NULL) *exact = false;
	if (!datapump_linear_fragment_is_ready(p, receive, transmit, count))
		return DPSTAT_ERROR;
	if (!g711_law_is_valid(law) || codewords == NULL || output == NULL || exact == NULL) {
		memset(transmit, 0, (size_t)count * sizeof(int16_t));
		return DPSTAT_ERROR;
	}
	_Static_assert(MODEM_FRAG <= DS0_PROBE_TAIL_CODEWORDS, "bounded handoff tail");
	int result;
	if (p->ds0_probe != NULL && !p->handoff_requested) {
		for (int i = 0; i < count; ++i)
			ds0_probe_sample(p->ds0_probe, codewords[i], available == NULL || available[i]);
		if (p->ds0_probe->selected != DS0_RATE_NONE) {
			struct dp *next = ds0_datapump_create_handoff(dp->modem, p->caller, p->ds0_probe);
			if (next == NULL) {
				memset(transmit, 0, (size_t)count * sizeof(int16_t));
				return DPSTAT_ERROR;
			}
			dp->modem->dp_requested = DP_DS0;
			if (p->legacy != NULL) p->legacy->op->delete(p->legacy);
			p->legacy = next;
			p->legacy_selected = p->handoff_requested = true;
			dp->modem->dsp_info.startup = MODEM_STARTUP_UNKNOWN;
			dp->modem->dsp_info.v8_lapm = false;
			dp->modem->dsp_info.pcm_role = MODEM_PCM_ROLE_NONE;
			memset(transmit, 0, (size_t)count * sizeof(int16_t));
			dp->status = DPSTAT_CHANGEDP;
			result = DPSTAT_CHANGEDP;
		} else result = process_v8(dp, receive, transmit, count);
	} else result = process_v8(dp, receive, transmit, count);
	return result;
}
