// SPDX-License-Identifier: BSD-3-Clause
#include "v91_datapump.h"
#include "v91_call.h"
#include "v8_datapump.h"
#include "datapump_link.h"
#include "datapump_media.h"
#include "datapump_timing.h"
#include <stdlib.h>
#include <string.h>

struct v91_datapump {
	struct dp dp;
	struct v91_call call;
	struct training_session training;
	bool started, ever_connected, closing, fallback, quiet_complete;
};
static int tx_tail_samples(const struct dp *dp);
static const struct dp_operations operations;

static struct v91_datapump *owner(const struct dp *dp)
{
	if (dp == NULL || dp->id != DP_V91 || dp->op != &operations ||
	    dp->dp_data == NULL || dp->modem == NULL)
		return NULL;
	struct v91_datapump *pump = dp->dp_data;
	return &pump->dp == dp ? pump : NULL;
}

static struct dp *create(struct modem *modem, enum DP_ID id, int caller,
	int rate, int fragment, const struct dp_operations *ops)
{
	if (modem == NULL || id != DP_V91 || (caller != 0 && caller != 1) ||
	    rate != MODEM_RATE || fragment != MODEM_FRAG || ops != &operations ||
	    modem->max_rate < 28000 || modem->min_rate > 64000 ||
	    !modem_get_sreg_flag(modem, SREG_V91_ENABLED))
		return NULL;
	struct v91_datapump *pump = calloc(1, sizeof(*pump));
	if (pump == NULL)
		return NULL;
	pump->dp = (struct dp){ .id = id, .modem = modem, .op = ops, .dp_data = pump };
	if (training_session_init(&pump->training, datapump_carrier_wait_ticks(modem)) !=
		TRAINING_SESSION_OK) {
		free(pump);
		return NULL;
	}
	if (modem->dp != NULL && modem->dp->id == DP_V8) {
		struct v8_pcm_handoff handoff;
		if (!v8_datapump_pcm_handoff(modem->dp, &handoff) || handoff.quick_connect) {
			free(pump);
			return NULL;
		}
		pump->training = handoff.training;
		/* The V.8 caller owns its post-CJ 75 ms. The answerer hands
		 * off at CJ and leaves that quiet interval to this owner. */
		pump->quiet_complete = caller != 0;
	}
	return &pump->dp;
}

static int destroy(struct dp *dp)
{
	struct v91_datapump *pump = owner(dp);
	if (pump == NULL)
		return -1;
	pump->dp.dp_data = NULL;
	free(pump);
	return 0;
}

static int retrain(struct dp *dp)
{
	struct v91_datapump *pump = owner(dp);
	if (pump == NULL || !pump->started || pump->closing || pump->call.failure)
		return -1;
	if (training_session_is_retraining(&pump->training))
		return 0;
	if (!training_session_is_online(&pump->training) ||
	    !v91_call_retrain(&pump->call) ||
	    training_session_start_retrain(&pump->training, TRAINING_LOCAL_RETRAIN) !=
		TRAINING_SESSION_OK)
		return -1;
	dp->status = DPSTAT_OK;
	return 0;
}

static int hangup(struct dp *dp)
{
	struct v91_datapump *pump = owner(dp);
	if (pump == NULL)
		return -1;
	pump->closing = true;
	return !pump->started || v91_call_hangup(&pump->call) ? 0 : -1;
}

static const char *progress(const struct dp *dp)
{
	struct v91_datapump *pump = owner(dp);
	if (pump == NULL || !pump->started)
		return "V.91/initializing";
	if (pump->closing)
		return "V.91/cleardown";
	if (pump->fallback)
		return "V.91/fallback-V.34";
	switch (pump->call.rx_stage) {
	case V91_RX_INFO: return "V.91/INFO";
	case V91_RX_J: return "V.91/J-DIL-request";
	case V91_RX_EM: return "V.91/wait-DIL";
	case V91_RX_DIL: return "V.91/impairment-learning";
	case V91_RX_CP: return "V.91/CP";
	case V91_RX_B1: return "V.91/B1";
	case V91_RX_DATA: return pump->call.tx_stage == V91_TX_DATA ?
		"V.91/data" : "V.91/wait-transmit-data";
	}
	return "V.91/invalid-state";
}

static struct dp_link_rates link_rates(const struct dp *dp)
{
	struct v91_datapump *pump = owner(dp);
	struct dp_link_rates rates = { 0 };
	if (pump == NULL || !pump->started)
		return rates;
	rates.transmit_baud = rates.receive_baud = 8000;
	if (pump->call.tx_stage >= V91_TX_B1)
		rates.transmit_bps = (pump->call.encoder.mapping.frame_bits * 8000u + 3u) / 6u;
	if (pump->call.rx_stage >= V91_RX_CP)
		rates.receive_bps = (pump->call.rx_mapping.frame_bits * 8000u + 3u) / 6u;
	return rates;
}

static void source(void *context, uint8_t *bits, size_t count)
{
	struct v91_datapump *pump = context;
	datapump_source_bits(&pump->dp, training_session_is_online(&pump->training) && !pump->closing, bits, count);
}

static int process_g711(struct dp *dp, enum g711_law law, const uint8_t *input,
	void *receive_samples, const bool *available, uint8_t *output,
	void *transmit_samples, int count, bool *exact)
{
	struct v91_datapump *pump = owner(dp);
	int16_t *linear = transmit_samples;
	if (exact != NULL)
		*exact = false;
	if (pump == NULL || !g711_law_is_valid(law) || input == NULL || output == NULL ||
	    receive_samples == NULL || linear == NULL || exact == NULL ||
	    count <= 0 || count > MODEM_FRAG) {
		datapump_mute_g711(law, output, linear, count);
		return DPSTAT_ERROR;
	}
	if (pump->closing && !pump->started) {
		datapump_mute_g711(law, output, linear, count);
		*exact = true;
		dp->status = DPSTAT_DISCONNECT;
		return DPSTAT_DISCONNECT;
	}
	if (!pump->started) {
		if (!v91_call_init(&pump->call, law))
			return DPSTAT_ERROR;
		unsigned maximum = dp->modem->max_rate * 6u / 8000u;
		pump->call.maximum_frame_bits = maximum < 48 ? maximum : 48;
		if (pump->quiet_complete)
			pump->call.tx_stage = V91_TX_EZ;
		pump->started = true;
	}
	if (pump->call.local_info.law != law) {
		modem_note_disconnect(dp->modem, true, "V.91 media law changed");
		datapump_mute_g711(law, output, linear, count);
		return DPSTAT_ERROR;
	}
	for (int i = 0; i < count; ++i) {
		if (!pump->closing && !training_session_is_online(&pump->training) &&
		    training_session_advance(&pump->training, 1) != TRAINING_SESSION_OK) {
			modem_note_disconnect(dp->modem, true, "V.91 carrier-wait deadline");
			datapump_mute_g711(law, output, linear, count);
			return DPSTAT_ERROR;
		}
		struct v91_event event;
		output[i] = g711_encode(law, 0);
		bool ok = v91_call_process(&pump->call, input[i], available == NULL || available[i],
			&output[i], source, pump, &event);
		linear[i] = g711_decode(law, output[i]);
		if (!ok) {
			bool remote = event.failure == V91_FAILURE_PEER_CLEARDOWN;
			bool closed = pump->closing || remote;
			datapump_mute_g711(law, output + i + 1, linear + i + 1, count - i - 1);
			*exact = true;
			if (!closed && !pump->ever_connected &&
			    (event.failure == V91_FAILURE_INFO_TIMEOUT ||
			     event.failure == V91_FAILURE_TRAINING_TIMEOUT ||
		     event.failure == V91_FAILURE_RECOVERY_EXHAUSTED)) {
				pump->fallback = true;
				dp->modem->dsp_info.pcm_role = MODEM_PCM_ROLE_NONE;
				dp->modem->dp_requested = DP_V34;
				dp->status = DPSTAT_CHANGEDP;
				return DPSTAT_CHANGEDP;
			}
			modem_note_disconnect(dp->modem, !remote, v91_failure_name(event.failure));
			dp->status = closed ? DPSTAT_DISCONNECT : DPSTAT_ERROR;
			return dp->status;
		}
		if (event.retraining && training_session_is_online(&pump->training)) {
			training_session_start_retrain(&pump->training, TRAINING_REMOTE_RETRAIN);
			dp->status = DPSTAT_OK;
		}
		if (event.online && !training_session_is_online(&pump->training)) {
			struct dp_link_rates rates = link_rates(dp);
			if (rates.receive_bps < dp->modem->min_rate ||
			    !datapump_set_link_rates(dp, rates.transmit_bps, rates.receive_bps) ||
			    !datapump_complete_training(dp, &pump->training)) {
				modem_note_disconnect(dp->modem, true, "V.91 selected rate rejected");
				datapump_mute_g711(law, output, linear, count);
				return DPSTAT_ERROR;
			}
			pump->ever_connected = true;
		}
		if (event.count && !datapump_deliver_bits(dp,
			training_session_is_online(&pump->training), event.bits, event.count)) {
			datapump_mute_g711(law, output, linear, count);
			return DPSTAT_ERROR;
		}
	}
	*exact = true;
	return DPSTAT_OK;
}

static int process_linear(struct dp *dp, void *input, void *output, int count)
{
	struct v91_datapump *pump = owner(dp);
	uint8_t receive[MODEM_FRAG], transmit[MODEM_FRAG];
	bool exact;
	if (!datapump_linear_fragment_is_ready(pump, input, output, count))
		return DPSTAT_ERROR;
	enum g711_law law = pump->started ? pump->call.local_info.law : G711_LAW_PCMU;
	for (int i = 0; i < count; ++i)
		receive[i] = g711_encode(law, ((int16_t *)input)[i]);
	return process_g711(dp, law, receive, input, NULL, transmit, output, count, &exact);
}

static const struct dp_operations operations = {
	.name = "PCM V.91", .create = create, .delete = destroy,
	.process = process_linear, .process_g711 = process_g711,
	.retrain = retrain, .hangup = hangup, .progress = progress, .link_rates = link_rates,
	.tx_tail_samples = tx_tail_samples,
};
const struct dp_operations *v91_datapump_operations(enum DP_ID id)
{
	return id == DP_V91 ? &operations : NULL;
}

static int tx_tail_samples(const struct dp *dp)
{
	const struct v91_datapump *p = owner(dp);
	return p != NULL && training_session_is_online(&p->training) &&
		p->call.tx_stage == V91_TX_DATA && !p->call.recovering ?
		V90_PCM_ENCODER_TAIL_SAMPLES : -1;
}
