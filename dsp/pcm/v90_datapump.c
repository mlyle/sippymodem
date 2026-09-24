// SPDX-License-Identifier: BSD-3-Clause

#include "v90_datapump.h"

#include "datapump_link.h"
#include "datapump_timing.h"
#include "memory_region.h"
#include "modem.h"
#include "modem_param.h"
#include "pcm_call.h"
#include "training_session.h"
#include "v8_datapump.h"
#include "v34_data_quality.h"
#include "v34_datapump.h"
#include "v90_host_echo.h"
#include "v90_info1.h"
#include "v90_mp.h"
#include "v90_profile.h"
#include "v92_profile.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct v90_datapump {
	struct dp dp;
	struct pcm_call call;
	struct training_session training;
	struct v34_data_quality upstream_quality;
	struct pcm_v34_fallback_handoff fallback;
	struct v8_pcm_handoff startup;
	struct v90_host_echo *host_echo;
	enum v90_modem_role role;
	enum pcm_protocol protocol;
	enum g711_law media_law;
	uint8_t maximum_pcm_upstream_data_rate_number;
	bool caller;
	bool call_started;
	bool fallback_ready;
	bool v8_fallback_ready;
	bool host_echo_margin_evaluated;
	bool host_echo_delay_seeded;
	bool upstream_quality_ready;
};

static struct dp *create_v90(
	struct modem *modem, enum DP_ID id, int caller, int sample_rate,
	int max_fragment, struct dp_operations *operations);
static int delete_v90(struct dp *dp);
static int process_v90(
	struct dp *dp, void *receive, void *transmit, int count);
static int process_v90_g711(
	struct dp *dp, enum g711_law law,
	const uint8_t *receive_codewords, void *receive_samples,
	const bool *received_mask,
	uint8_t *transmit_codewords, void *transmit_samples,
	int count, bool *exact_output);
static int retrain_v90(struct dp *dp);
static int hangup_v90(struct dp *dp);

static struct dp_operations v90_operations = {
	.name = "PCM V.90/V.92",
	.create = create_v90,
	.delete = delete_v90,
	.process = process_v90,
	.process_g711 = process_v90_g711,
	.retrain = retrain_v90,
	.hangup = hangup_v90,
};

static bool protocol_from_id(enum DP_ID id, enum pcm_protocol *protocol)
{
	if (protocol == NULL)
		return false;
	if (id == DP_V90)
		*protocol = PCM_PROTOCOL_V90;
	else if (id == DP_V92)
		*protocol = PCM_PROTOCOL_V92;
	else
		return false;
	return true;
}

static bool v92_profile_requested(
	struct modem *modem, uint8_t *maximum_data_rate_number)
{
	int configured, enabled;

	if (modem == NULL || maximum_data_rate_number == NULL)
		return false;
	enabled = modem_get_param(modem, MDMPRM_V92_PCM_UPSTREAM);
	if (enabled != 0 && enabled != 1)
		return false;
	configured = modem_get_sreg(modem, SREG_V92_PCM_UPSTREAM_MAX_DRN);
	if (configured < 0 ||
	    configured > V92_PROFILE_MAX_UPSTREAM_DATA_RATE_NUMBER)
		return false;
	*maximum_data_rate_number = enabled == 0 ? 0u : configured == 0 ?
		V92_PROFILE_MAX_UPSTREAM_DATA_RATE_NUMBER :
		(uint8_t)configured;
	return true;
}

static bool role_from_modem(
	const struct modem *modem, enum v90_modem_role *role)
{
	if (modem == NULL || role == NULL)
		return false;
	if (modem->dsp_info.pcm_role == MODEM_PCM_ROLE_ANALOG)
		*role = V90_ANALOG_MODEM;
	else if (modem->dsp_info.pcm_role == MODEM_PCM_ROLE_DIGITAL)
		*role = V90_DIGITAL_MODEM;
	else
		return false;
	return true;
}

static struct v90_datapump *v90_from_dp(struct dp *dp)
{
	struct v90_datapump *datapump;
	enum pcm_protocol protocol;
	enum v90_modem_role role;
	bool closed;
	bool disconnecting;

	if (dp == NULL || !protocol_from_id(dp->id, &protocol) ||
	    dp->op != &v90_operations ||
	    dp->modem == NULL || dp->dp_data == NULL ||
	    !role_from_modem(dp->modem, &role))
		return NULL;
	datapump = dp->dp_data;
	closed = datapump->call_started && datapump->call.stage == PCM_CALL_CLOSED;
	disconnecting = datapump->call_started && pcm_call_is_disconnecting(&datapump->call);
	if (&datapump->dp != dp || datapump->role != role ||
	    (datapump->v8_fallback_ready &&
	     (!datapump->startup.quick_connect || !datapump->call_started ||
	      datapump->call.stage != PCM_CALL_QC)) ||
	    (closed && dp->status != DPSTAT_DISCONNECT) ||
	    datapump->protocol != protocol ||
	    ((role == V90_DIGITAL_MODEM) !=
	     (datapump->host_echo != NULL)) ||
	    (datapump->host_echo_margin_evaluated &&
	     (!datapump->call_started || datapump->host_echo == NULL ||
	      !v90_host_echo_is_ready(datapump->host_echo))) ||
	    (datapump->host_echo_delay_seeded &&
	     (datapump->host_echo == NULL || !datapump->call_started ||
	      !datapump->call.round_trip_delay_stored)) ||
	    !training_session_is_valid(&datapump->training) ||
	    !v34_data_quality_is_valid(&datapump->upstream_quality) ||
	    (datapump->upstream_quality_ready &&
	     (role != V90_DIGITAL_MODEM ||
	      !pcm_call_uses_conventional_upstream(&datapump->call))) ||
	    (datapump->fallback_ready &&
	     (role != V90_DIGITAL_MODEM ||
	      datapump->fallback.restart.protocol != protocol ||
	      !datapump->call_started ||
	      !v34_phase2_handoff_is_valid(&datapump->fallback.phase2) ||
	      memcmp(
		      &datapump->fallback.training, &datapump->training,
		      sizeof(datapump->training)) != 0 ||
	      !pcm_phase2_is_ready_for_v34_fallback(
		      &datapump->call.state.phase2))) ||
	    (datapump->call_started &&
	     (!g711_law_is_valid(datapump->media_law) ||
	      !pcm_call_is_valid(&datapump->call) ||
	      datapump->call.protocol != datapump->protocol ||
	      datapump->call.local_role != datapump->role ||
	      datapump->call.law != datapump->media_law ||
	      (datapump->call.failed &&
	       !training_session_has_failed(&datapump->training)))))
		return NULL;
	/* The started call was validated above. Compare status with its owned
	 * state instead of rescanning the nested call for every predicate. */
	if (!disconnecting && !training_session_has_failed(&datapump->training)) {
		bool online = datapump->call_started &&
			pcm_call_is_online_owned(&datapump->call);
		bool retraining = training_session_is_retraining(&datapump->training);

		if ((training_session_is_online(&datapump->training) != online &&
		     !(retraining && !online)) ||
		    (online && dp->status != DPSTAT_CONNECT) ||
		    (!online && dp->status == DPSTAT_CONNECT && !retraining))
			return NULL;
	}
	return datapump;
}

bool v90_datapump_v34_fallback_handoff(
	struct dp *dp, struct pcm_v34_fallback_handoff *handoff)
{
	struct v90_datapump *datapump = v90_from_dp(dp);

	if (datapump == NULL || handoff == NULL ||
	    memory_regions_overlap(
		    datapump, sizeof(*datapump), handoff, sizeof(*handoff)) ||
	    !datapump->fallback_ready)
		return false;
	*handoff = datapump->fallback;
	return true;
}

bool v90_datapump_v8_fallback_handoff(struct dp *dp, struct training_session *training)
{
	struct v90_datapump *datapump = v90_from_dp(dp);

	if (datapump == NULL || training == NULL || !datapump->v8_fallback_ready ||
	    training_session_has_failed(&datapump->training) ||
	    memory_regions_overlap(datapump, sizeof(*datapump), training, sizeof(*training)))
		return false;
	*training = datapump->training;
	return true;
}

static struct dp *create_v90(
	struct modem *modem, enum DP_ID id, int caller, int sample_rate,
	int max_fragment, struct dp_operations *operations)
{
	struct v90_datapump *datapump;
	struct pcm_downstream_rate_selection downstream;
	enum pcm_protocol protocol;
	enum v90_modem_role role;
	uint8_t maximum_pcm_upstream_data_rate_number = 0;
	bool from_v34;
	bool from_v8;

	if (modem == NULL || !protocol_from_id(id, &protocol) ||
	    (caller != 0 && caller != 1) ||
	    sample_rate != MODEM_RATE || max_fragment != MODEM_FRAG ||
	    operations != &v90_operations || operations->use_count == INT_MAX ||
	    !role_from_modem(modem, &role) ||
	    (protocol == PCM_PROTOCOL_V92 &&
	     !v92_profile_requested(
		     modem, &maximum_pcm_upstream_data_rate_number)) ||
	    !pcm_downstream_rates_select(
		    &downstream, modem->min_rate, modem->max_rate))
		return NULL;
	datapump = calloc(1, sizeof(*datapump));
	if (datapump == NULL)
		return NULL;
	from_v8 = protocol == PCM_PROTOCOL_V92 && modem->dp != NULL &&
		modem->dp->id == DP_V8;
	from_v34 = modem->dp != NULL && modem->dp->id == DP_V34;
	if ((from_v8 && !v8_datapump_pcm_handoff(modem->dp, &datapump->startup)) ||
	    (protocol == PCM_PROTOCOL_V92 && modem_get_param(modem, MDMPRM_V92_QUICK_CONNECT) &&
	     !from_v8 && !from_v34)) {
		free(datapump);
		return NULL;
	}
	datapump->role = role;
	datapump->protocol = protocol;
	datapump->caller = caller != 0;
	datapump->maximum_pcm_upstream_data_rate_number =
		maximum_pcm_upstream_data_rate_number;
	if (role == V90_DIGITAL_MODEM) {
		datapump->host_echo = malloc(sizeof(*datapump->host_echo));
		if (datapump->host_echo == NULL ||
		    v90_host_echo_init(datapump->host_echo) != V90_HOST_ECHO_OK) {
			free(datapump->host_echo);
			free(datapump);
			return NULL;
		}
	}
	if (training_session_init(
		    &datapump->training,
		    datapump_carrier_wait_ticks(modem)) !=
	    TRAINING_SESSION_OK ||
	    v34_data_quality_init(&datapump->upstream_quality) !=
		    V34_DATA_QUALITY_OK) {
		free(datapump->host_echo);
		free(datapump);
		return NULL;
	}
	if (from_v8)
		datapump->training = datapump->startup.training;
	if (from_v34) {
		if (role != V90_DIGITAL_MODEM ||
		    !v34_datapump_pcm_retrain_handoff(
			    modem->dp, &datapump->call, &datapump->training) ||
		    datapump->call.protocol != protocol) {
			free(datapump->host_echo);
			free(datapump);
			return NULL;
		}
		datapump->media_law = datapump->call.law;
		datapump->call_started = true;
	}
	datapump->dp = (struct dp){
		.id = id,
		.modem = modem,
		.status = DPSTAT_OK,
		.op = operations,
		.dp_data = datapump,
	};
	operations->use_count++;
	return &datapump->dp;
}

static int delete_v90(struct dp *dp)
{
	struct v90_datapump *datapump = v90_from_dp(dp);

	if (datapump == NULL || v90_operations.use_count <= 0)
		return -1;
	v90_operations.use_count--;
	datapump->dp.dp_data = NULL;
	free(datapump->host_echo);
	free(datapump);
	return 0;
}

static int retrain_v90(struct dp *dp)
{
	struct v90_datapump *datapump = v90_from_dp(dp);
	struct training_session training;
	struct pcm_call call;

	if (datapump == NULL || datapump->call.stage == PCM_CALL_CLOSED)
		return -1;
	if (training_session_is_retraining(&datapump->training))
		return 0;
	if (!training_session_is_online(&datapump->training))
		return -1;
	training = datapump->training;
	call = datapump->call;
	if (pcm_call_start_retrain(&call) != PCM_CALL_OK ||
	    training_session_start_retrain(
		    &training, TRAINING_LOCAL_RETRAIN) != TRAINING_SESSION_OK)
		return -1;
	/* Already transmitted data still returns while the peer starts Tone A.
	 * Retain a qualified in-call model through the new ranging exchange. */
	if (datapump->host_echo != NULL &&
	    !v90_host_echo_is_ready(datapump->host_echo)) {
		if (v90_host_echo_init(datapump->host_echo) != V90_HOST_ECHO_OK)
			return -1;
		datapump->host_echo_margin_evaluated = false;
		datapump->host_echo_delay_seeded = false;
	}
	datapump->call = call;
	datapump->training = training;
	datapump->upstream_quality_ready = false;
	return 0;
}

static bool start_parameter_exchange(struct dp *dp, bool fast, bool request_silence)
{
	struct v90_datapump *datapump = v90_from_dp(dp);
	struct training_session training;
	struct pcm_call call;

	if (datapump == NULL || !datapump->call_started ||
	    !training_session_is_online(&datapump->training))
		return false;
	training = datapump->training;
	call = datapump->call;
	if (!fast) {
		if (datapump->role == V90_ANALOG_MODEM)
			call.v92_analog_phase4_configuration.suvu.request_silence = request_silence;
		else
			call.v92_digital_phase4_configuration.suvd.request_silence = request_silence;
	}
	if ((fast ? pcm_call_start_fast_exchange(&call) : pcm_call_start_rate_renegotiation(&call)) != PCM_CALL_OK ||
	    training_session_start_retrain(&training, TRAINING_LOCAL_RETRAIN) !=
		    TRAINING_SESSION_OK)
		return false;
	/* The rate owner has copied this request into its running SUVu/SUVd.
	 * Do not make a one-exchange pause sticky for later peer requests. */
	call.v92_analog_phase4_configuration.suvu.request_silence =
		datapump->call.v92_analog_phase4_configuration.suvu.request_silence;
	call.v92_digital_phase4_configuration.suvd.request_silence =
		datapump->call.v92_digital_phase4_configuration.suvd.request_silence;
	datapump->call = call;
	datapump->training = training;
	return true;
}

bool v90_datapump_start_rate_renegotiation(struct dp *dp, bool request_silence)
{
	return start_parameter_exchange(dp, false, request_silence);
}

bool v90_datapump_start_fast_exchange(struct dp *dp)
{
	return start_parameter_exchange(dp, true, false);
}

static bool observe_upstream_quality(struct v90_datapump *datapump)
{
	struct v90_digital_path *path = &datapump->call.state.data_path.digital;
	struct v34_data_quality next = datapump->upstream_quality;
	struct v34_data_line_receiver *receiver;
	enum v34_data_quality_result result;

	/* Both sessions share the conventional V.34 upstream receiver. Do not
	 * carry a quality baseline across CP/E/B1, rate renegotiation or full
	 * retrain; only the call's bounded automatic-retrain budget survives. */
	if (!pcm_call_uses_conventional_upstream(&datapump->call) ||
	    datapump->role != V90_DIGITAL_MODEM ||
	    datapump->call.stage != PCM_CALL_DATA_PATH ||
	    !training_session_is_online(&datapump->training) ||
	    path->rate_renegotiation_active || path->cleardown_requested ||
	    path->upstream_stage != V90_DIGITAL_PATH_UPSTREAM_DATA ||
	    path->downstream_stage != V90_DIGITAL_PATH_DOWNSTREAM_DATA) {
		datapump->upstream_quality_ready = false;
		return true;
	}
	receiver = &path->upstream.data;
	if (!datapump->upstream_quality_ready) {
		if (v34_data_quality_start(
			    &next, receiver->geometry.symbol_rate,
			    receiver->geometry.primary_data_rate_2400_bps_units,
			    &receiver->line.equalizer.quality) != V34_DATA_QUALITY_OK)
			return false;
		datapump->upstream_quality_ready = true;
	} else {
		result = v34_data_quality_observe(
			&next, &receiver->line.equalizer.quality);
		if (result == V34_DATA_QUALITY_FREEZE_TIMING ||
		    result == V34_DATA_QUALITY_RESUME_TIMING) {
			if (v34_receive_timing_set_data_frozen(
				    &receiver->line.timing,
				    result == V34_DATA_QUALITY_FREEZE_TIMING) !=
			    V34_RECEIVE_TIMING_OK)
				return false;
		} else if (result == V34_DATA_QUALITY_RETRAIN) {
			if (retrain_v90(&datapump->dp) != 0)
				return false;
		} else if (result != V34_DATA_QUALITY_OK) {
			return false;
		}
	}
	datapump->upstream_quality = next;
	return true;
}

static int process_v90(
	struct dp *dp, void *receive, void *transmit, int count)
{
	(void)dp;
	(void)receive;
	if (transmit != NULL && count > 0 && count <= MODEM_FRAG)
		memset(transmit, 0, (size_t)count * sizeof(int16_t));
	return DPSTAT_ERROR;
}

static bool source_upstream_frame(
	void *context, const struct v34_mapping_frame *request,
	uint8_t *primary_bits, uint8_t *auxiliary_bit)
{
	struct v90_datapump *datapump = context;
	uint8_t bits[V34_MAX_MAPPING_FRAME_BITS + 1];
	size_t bit_count;

	if (request == NULL || primary_bits == NULL ||
	    request->primary_bits > V34_MAX_MAPPING_FRAME_BITS)
		return false;
	bit_count = request->primary_bits + (auxiliary_bit != NULL ? 1u : 0u);
	if (datapump == NULL ||
	    !datapump_source_bits(
		    &datapump->dp,
		    training_session_is_online(&datapump->training),
		    bits, bit_count))
		return false;
	memcpy(primary_bits, bits, request->primary_bits);
	if (auxiliary_bit != NULL)
		*auxiliary_bit = bits[request->primary_bits];
	return true;
}

static bool source_pcm_frame(
	void *context, uint8_t *bits, size_t bit_count)
{
	struct v90_datapump *datapump = context;

	/* Downstream and V.92 upstream use the same host-bit contract, despite
	 * their different six- and twelve-symbol mapping frames. */
	if (datapump == NULL || bit_count == 0 ||
	    bit_count > V92_PCM_MAPPING_MAX_BITS)
		return false;
	return datapump_source_bits(
		&datapump->dp,
		training_session_is_online(&datapump->training),
		bits, bit_count);
}

static bool deliver_bits(
	struct v90_datapump *datapump,
	const uint8_t *bits, size_t bit_count)
{
	uint8_t copied[V34_MAX_MAPPING_FRAME_BITS + 1];

	if (bits == NULL || bit_count > sizeof(copied))
		return false;
	for (size_t index = 0; index < bit_count; ++index) {
		if (bits[index] > 1)
			return false;
		copied[index] = bits[index];
	}
	if (!training_session_is_online(&datapump->training) ||
	    datapump->dp.modem->put_bits == NULL)
		return true;
	return modem_put_bits(
		       datapump->dp.modem, 1, copied, (int)bit_count) ==
	       (int)bit_count;
}

static bool deliver_event(
	struct v90_datapump *datapump,
	const struct pcm_call_data_event *event)
{
	uint8_t bits[V34_MAX_MAPPING_FRAME_BITS + 1];
	size_t bit_count;

	if (event->kind == PCM_CALL_DATA_NONE)
		return true;
	if (event->kind == PCM_CALL_DOWNSTREAM_FRAME) {
		if (datapump->role != V90_ANALOG_MODEM)
			return false;
		return deliver_bits(
			datapump, event->frame.downstream.bits,
			event->frame.downstream.bit_count);
	}
	if (event->kind == PCM_CALL_PCM_UPSTREAM_FRAME) {
		const struct v92_upstream_data_frame *frame = &event->frame.pcm_upstream;

		if (datapump->protocol != PCM_PROTOCOL_V92 ||
		    datapump->role != V90_DIGITAL_MODEM || frame->b1u || frame->ambiguous)
			return false;
		if (!training_session_is_online(&datapump->training))
			return true;
		return deliver_bits(datapump, frame->bits, frame->bit_count);
	}
	if (event->kind != PCM_CALL_UPSTREAM_FRAME ||
	    datapump->role != V90_DIGITAL_MODEM ||
	    event->frame.upstream.primary_bit_count >
		    V34_MAX_MAPPING_FRAME_BITS ||
	    event->frame.upstream.auxiliary_bit_count > 1)
		return false;
	bit_count = event->frame.upstream.primary_bit_count;
	memcpy(bits, event->frame.upstream.primary_bits, bit_count);
	if (event->frame.upstream.auxiliary_bit_count != 0)
		bits[bit_count++] = event->frame.upstream.auxiliary_bit;
	return deliver_bits(datapump, bits, bit_count);
}

static bool enter_link(struct v90_datapump *datapump)
{
	bool retrained;
	uint32_t transmit_rate;
	uint32_t receive_rate;

	if (!pcm_call_get_data_rates(
		    &datapump->call, &transmit_rate, &receive_rate) ||
	    !datapump_set_link_rates(
		    &datapump->dp, transmit_rate, receive_rate))
		return false;
	retrained = training_session_is_retraining(&datapump->training);
	if (training_session_connect(&datapump->training) !=
	    TRAINING_SESSION_OK)
		return false;
	datapump->dp.modem->dsp_info.upstream_mode =
		pcm_call_uses_conventional_upstream(&datapump->call) ?
		MODEM_UPSTREAM_V34 : MODEM_UPSTREAM_PCM;
	datapump->dp.status = DPSTAT_CONNECT;
	if (retrained)
		modem_dp_reconnected(datapump->dp.modem);
	else
		modem_dp_connected(datapump->dp.modem);
	return true;
}

static int hangup_v90(struct dp *dp)
{
	struct v90_datapump *datapump = v90_from_dp(dp);

	if (datapump == NULL || !datapump->call_started)
		return -1;
	return pcm_call_start_cleardown(&datapump->call) == PCM_CALL_OK ? 0 : -1;
}

static bool start_call(
	struct v90_datapump *datapump, enum g711_law media_law)
{
	struct pcm_call_configuration configuration;
	const struct v90_upstream_policy upstream = {
		.minimum_bit_rate =
			datapump->dp.modem->v90_upstream_min_rate,
		.maximum_bit_rate =
			datapump->dp.modem->v90_upstream_max_rate,
		.maximum_symbol_rate =
			datapump->dp.modem->v90_upstream_max_baud,
		/* 3429 symbols/s is optional at both PCM endpoints.  A physical
		 * analogue answerer in the interoperability corpus advertises it in
		 * INFO0a but selects conventional V.34 whenever a digital caller's
		 * INFO1d includes that probe.  Keep the mandatory 3000/3200 probes
		 * and all ordinary digital-answerer behavior unchanged. */
		.suppress_optional_info1d_3429 =
			datapump->protocol == PCM_PROTOCOL_V90 &&
			datapump->role == V90_DIGITAL_MODEM &&
			datapump->caller,
	};

	if (datapump->call_started)
		return datapump->media_law == media_law;
	/* RTP G.711 characters are this integrated datapump's digital
	 * interface.  Negotiate that law and pass its V.90 U-codes exactly;
	 * transcoding would collapse distinct constellation points. */
	if ((datapump->protocol == PCM_PROTOCOL_V90 &&
	     v90_profile_init(
		     &configuration, datapump->role, media_law,
		     datapump->dp.modem->min_rate,
		     datapump->dp.modem->max_rate,
		     &upstream) != V90_PROFILE_OK) ||
	    (datapump->protocol == PCM_PROTOCOL_V92 &&
	     v92_profile_init(
		     &configuration, datapump->role, media_law,
		     datapump->dp.modem->min_rate,
		     datapump->dp.modem->max_rate,
		     datapump->maximum_pcm_upstream_data_rate_number, &upstream) !=
		     V92_PROFILE_OK))
		return false;
	/* This host owns delayed echo acquisition; bare PCM calls need not
	 * lengthen their default training interval to accommodate that owner. */
	if (datapump->protocol == PCM_PROTOCOL_V92 && datapump->host_echo != NULL)
		configuration.phase3.v92_digital.trn1d_symbols =
			V90_PHASE3_TRN1D_SYMBOLS;
	if ((datapump->startup.quick_connect ?
	     pcm_call_init_qc(&datapump->call, &configuration,
		     &datapump->startup.local, &datapump->startup.peer) :
	     pcm_call_init(&datapump->call, &configuration)) != PCM_CALL_OK)
		return false;
	for (unsigned int index = 0; index < datapump->startup.tail_count; ++index) {
		struct pcm_call_observation observation;
		int16_t input = datapump->startup.receive_tail[index];
		uint8_t received = media_law == G711_LAW_PCMU ?
			g711_encode_pcmu(input) : g711_encode_pcma(input);
		uint8_t silence = media_law == G711_LAW_PCMU ?
			g711_encode_pcmu(0) : g711_encode_pcma(0);
		uint8_t output;

		/* V.8 already emitted these quiet samples and charged the training
		 * clock. Replay their RX history before accepting the next fragment. */
		if (pcm_call_octet(&datapump->call, received, input,
			    &output, &observation) != PCM_CALL_OK ||
		    output != silence)
			return false;
		if (datapump->host_echo != NULL &&
		    v90_host_echo_advance(datapump->host_echo, V90_HOST_ECHO_BYPASS,
			    input, input, 0) != V90_HOST_ECHO_OK)
			return false;
	}
	datapump->media_law = media_law;
	datapump->call_started = true;
	return true;
}

static bool process_octet(
	struct v90_datapump *datapump, uint8_t received,
	int16_t received_sample, int16_t host_sample, bool received_available,
	uint8_t *transmitted, bool *connected, bool *fallback, bool *disconnected)
{
	struct pcm_call_data_sources sources = {
		.upstream = source_upstream_frame,
		.upstream_context = datapump,
		.downstream = source_pcm_frame,
		.downstream_context = datapump,
		.pcm_upstream = source_pcm_frame,
		.pcm_upstream_context = datapump,
	};
	struct pcm_call_data_event event = { 0 };
	enum pcm_call_result result;
	bool was_online = training_session_is_online(&datapump->training);
	bool was_qc = datapump->call.stage == PCM_CALL_QC;

	/* The datapump is the serialized owner of a call that every public
	 * transition validates.  Consult its stage directly here instead of
	 * recursively validating the complete PCM graph several times per
	 * network character. */
	if (datapump->call.stage == PCM_CALL_PHASE4 &&
	    pcm_call_phase4_is_ready(&datapump->call) &&
	    pcm_call_start_data_path(&datapump->call) != PCM_CALL_OK)
		return false;
	if (datapump->call.stage == PCM_CALL_DATA_PATH) {
		result = pcm_call_data_octet_owned(
			&datapump->call, received, received_sample, host_sample, received_available, &sources,
			transmitted, &event);
	} else {
		struct pcm_call_observation observation;

		result = pcm_call_octet_owned(
			&datapump->call, received, received_sample,
			transmitted, &observation);
	}
	if (result == PCM_CALL_V34_FALLBACK_READY) {
		if (datapump->fallback_ready ||
		    pcm_call_v34_fallback_handoff(
			    &datapump->call,
			    &datapump->fallback.phase2,
			    &datapump->fallback.restart) != PCM_CALL_OK)
			return false;
		datapump->fallback.training = datapump->training;
		datapump->fallback_ready = true;
		*fallback = true;
		return true;
	}
	if (result == PCM_CALL_V8_FALLBACK_READY) {
		datapump->v8_fallback_ready = true;
		*transmitted = datapump->media_law == G711_LAW_PCMU ?
			g711_encode_pcmu(0) : g711_encode_pcma(0);
		*fallback = true;
		return true;
	}
	if (result == PCM_CALL_CLEARDOWN_COMPLETE) {
		datapump->dp.status = DPSTAT_DISCONNECT;
		*disconnected = true;
		return true;
	}
	if (result != PCM_CALL_OK)
		return false;
	if (was_qc && datapump->call.stage == PCM_CALL_PHASE2)
		datapump->dp.modem->dsp_info.startup = MODEM_STARTUP_QC;
	if (was_online &&
	    datapump->call.retrain_active && !pcm_call_is_online_owned(&datapump->call) &&
	    training_session_start_retrain(
		    &datapump->training, TRAINING_REMOTE_RETRAIN) !=
		    TRAINING_SESSION_OK)
		return false;
	if (!training_session_is_online(&datapump->training) &&
	    pcm_call_is_online_owned(&datapump->call)) {
		if (!enter_link(datapump))
			return false;
		*connected = true;
	}
	return deliver_event(datapump, &event) && observe_upstream_quality(datapump);
}

static void mute_output(
	enum g711_law law, uint8_t *codewords,
	int16_t *samples, int count)
{
	uint8_t silence = law == G711_LAW_PCMA ?
				  g711_encode_pcma(0) : g711_encode_pcmu(0);
	int16_t linear = law == G711_LAW_PCMA ?
		g711_decode_pcma(silence) : g711_decode_pcmu(silence);

	if (count <= 0 || count > MODEM_FRAG)
		return;
	if (codewords != NULL)
		memset(codewords, silence, (size_t)count);
	if (samples != NULL)
		for (int index = 0; index < count; ++index)
			samples[index] = linear;
}

static enum v90_host_echo_mode host_echo_mode(
	const struct v90_datapump *datapump)
{
	const struct pcm_call *call = &datapump->call;
	const struct pcm_phase3_digital_tx *transmit = NULL;

	if (datapump->host_echo == NULL)
		return V90_HOST_ECHO_BYPASS;
	/* Both upstream procedures make the analogue peer quiet during TRN1d.
	 * Learn from that interval only, then freeze while receiving the peer.
	 * Select the active Phase 3 owner before inspecting its common TX. */
	if (call->stage == PCM_CALL_PHASE3) {
		if (pcm_call_uses_conventional_upstream(call)) {
			if (call->state.phase3.digital.transmit_started)
				transmit = &call->state.phase3.digital.transmit;
		} else if (call->state.phase3.v92_digital.transmit_stage ==
				V92_PHASE3_DIGITAL_TRANSMIT_DOWNSTREAM) {
			transmit = &call->state.phase3.v92_digital.downstream;
		}
		if (transmit != NULL &&
		    transmit->stage == PCM_PHASE3_DIGITAL_TX_PREFIX &&
		    transmit->sequence.prefix.stage == PCM_PHASE3_DIGITAL_PREFIX_TRN1D)
			return V90_HOST_ECHO_TRAIN;
	}
	/* The upstream receiver owns physical samples through retrain
	 * ranging, both return markers, and the completed-return drain. A ready
	 * filter removes returning data or local Jd/DIL in those intervals;
	 * FILTER leaves samples unchanged while readiness is incomplete. */
	if (call->stage == PCM_CALL_PHASE2 ||
	    call->stage == PCM_CALL_PHASE3 ||
	    call->stage == PCM_CALL_PHASE3_MIXED ||
	    call->stage == PCM_CALL_PHASE4 ||
	    call->stage == PCM_CALL_DATA_PATH ||
	    call->stage == PCM_CALL_RETRAIN_SILENCE)
		return V90_HOST_ECHO_FILTER;
	return V90_HOST_ECHO_BYPASS;
}

static bool seed_host_echo_delay(struct v90_datapump *datapump)
{
	/* The seed belongs to this echo model. Retrain can replace the call's
	 * ranging measurement, including with zero, before the model resets. */
	if (datapump->host_echo == NULL || datapump->host_echo_delay_seeded ||
	    v90_host_echo_is_ready(datapump->host_echo) ||
	    !datapump->call.round_trip_delay_stored ||
	    datapump->call.round_trip_delay_samples == 0)
		return true;
	if (v90_host_echo_seed_phase2_delay(
		    datapump->host_echo,
		    datapump->call.round_trip_delay_samples) != V90_HOST_ECHO_OK)
		return false;
	datapump->host_echo_delay_seeded = true;
	return true;
}

static bool apply_host_echo_margin(struct v90_datapump *datapump)
{
	struct pcm_call limited;

	if (datapump->host_echo == NULL ||
	    !pcm_call_uses_conventional_upstream(&datapump->call) ||
	    datapump->host_echo_margin_evaluated ||
	    !v90_host_echo_is_ready(datapump->host_echo))
		return true;
	if (datapump->call.stage != PCM_CALL_PHASE3 &&
	    datapump->call.stage != PCM_CALL_PHASE3_MIXED)
		return false;
	if (!v90_host_echo_has_weak_margin(datapump->host_echo)) {
		datapump->host_echo_margin_evaluated = true;
		return true;
	}
	limited = datapump->call;
	if (!v90_mp_record_limit_maximum_upstream_rate(
		     &limited.digital_phase4_configuration.initial_mp,
		     V90_MP_MANDATORY_MAXIMUM_RATE_2400_BPS_UNITS) ||
	    !pcm_call_is_valid(&limited))
		return false;
	datapump->call = limited;
	datapump->host_echo_margin_evaluated = true;
	return true;
}

static int process_v90_g711(
	struct dp *dp, enum g711_law law,
	const uint8_t *receive_codewords, void *receive_samples_buffer,
	const bool *received_mask,
	uint8_t *transmit_codewords, void *transmit_samples_buffer,
	int count, bool *exact_output)
{
	struct v90_datapump *datapump = v90_from_dp(dp);
	const int16_t *receive_samples = receive_samples_buffer;
	int16_t *transmit_samples = transmit_samples_buffer;
	bool connected = false;
	bool fallback = false;
	bool disconnected = false;

	if (exact_output != NULL)
		*exact_output = false;
	if (datapump == NULL || !g711_law_is_valid(law) ||
	    receive_codewords == NULL || receive_samples == NULL ||
	    transmit_codewords == NULL || transmit_samples == NULL ||
	    exact_output == NULL || count <= 0 || count > MODEM_FRAG) {
		mute_output(law, transmit_codewords, transmit_samples, count);
		return DPSTAT_ERROR;
	}
	if (training_session_has_failed(&datapump->training)) {
		mute_output(law, transmit_codewords, transmit_samples, count);
		return DPSTAT_ERROR;
	}
	if (!start_call(datapump, law))
		goto failure;
	if (datapump->call.stage == PCM_CALL_CLOSED) {
		mute_output(law, transmit_codewords, transmit_samples, count);
		*exact_output = true;
		return DPSTAT_DISCONNECT;
	}
	for (int index = 0; index < count; ++index) {
		enum pcm_call_stage stage_before = datapump->call.stage;
		enum v90_host_echo_mode echo_mode = host_echo_mode(datapump);
		int16_t cleaned_sample = receive_samples[index];
		uint8_t pcm_transmit;

		/* An accepted hangup owns the line until its bounded protocol
		 * tail drains, even if the original CONNECT deadline expires. */
		if (!pcm_call_is_disconnecting(&datapump->call) &&
		    !training_session_is_online(&datapump->training) &&
		    training_session_advance(&datapump->training, 1) !=
			    TRAINING_SESSION_OK)
			goto failure;
		if (datapump->host_echo != NULL &&
		    v90_host_echo_predict(
			    datapump->host_echo, echo_mode,
			    receive_samples[index], &cleaned_sample) !=
			    V90_HOST_ECHO_OK)
			goto failure;
		if (!process_octet(
			    datapump, receive_codewords[index],
			    cleaned_sample, receive_samples[index], received_mask == NULL || received_mask[index],
			    &pcm_transmit, &connected, &fallback, &disconnected))
			goto failure;
		transmit_codewords[index] = pcm_transmit;
		transmit_samples[index] = law == G711_LAW_PCMA ?
			g711_decode_pcma(transmit_codewords[index]) :
			g711_decode_pcmu(transmit_codewords[index]);
		if (disconnected) {
			mute_output(law, transmit_codewords + index + 1,
				    transmit_samples + index + 1, count - index - 1);
			*exact_output = true;
			return DPSTAT_DISCONNECT;
		}
		if (datapump->host_echo != NULL) {
			if (v90_host_echo_advance_predicted(
				    datapump->host_echo, echo_mode,
				    receive_samples[index], cleaned_sample,
				    transmit_samples[index]) != V90_HOST_ECHO_OK)
				goto failure;
			/* Retain qualified cancellation through retrain ranging, but
			 * reacquire it for the new Phase 3/TRN1d epoch. Old margin and
			 * delay qualification must not survive that new training.
			 * Unqualified models still clear at the silence/tone handoff. */
			if ((stage_before == PCM_CALL_RETRAIN_SILENCE &&
			     datapump->call.stage != PCM_CALL_RETRAIN_SILENCE &&
			     !v90_host_echo_is_ready(datapump->host_echo)) ||
			    (stage_before == PCM_CALL_PHASE2 &&
			     datapump->call.stage != PCM_CALL_PHASE2 &&
			     datapump->call.retrain_active)) {
				if (v90_host_echo_init(datapump->host_echo) !=
				    V90_HOST_ECHO_OK)
					goto failure;
				datapump->host_echo_margin_evaluated = false;
				datapump->host_echo_delay_seeded = false;
			} else if (!apply_host_echo_margin(datapump)) {
				goto failure;
			}
			if (!seed_host_echo_delay(datapump))
				goto failure;
		}
		if (fallback) {
			int remaining = count - index - 1;

			if (remaining > 0 &&
			    training_session_advance(
				    &datapump->training,
				    (unsigned int)remaining) !=
				    TRAINING_SESSION_OK)
				goto failure;
			/* V.90 has completed the shared Phase 2 exchange.  Give the
			 * V.34 owner a complete bounded Phase 3/4 interval instead of
			 * charging it for the preceding PCM negotiation. */
			if (!datapump->v8_fallback_ready &&
			    training_session_transfer(&datapump->training) !=
				    TRAINING_SESSION_OK)
				goto failure;
			datapump->fallback.training = datapump->training;
			mute_output(
				law, transmit_codewords + index + 1,
				transmit_samples + index + 1, remaining);
			if (modem_set_param(
				    datapump->dp.modem, MDMPRM_DP_REQUESTED,
				    datapump->v8_fallback_ready ? 0 : DP_V34) != 0)
				goto failure;
			*exact_output = true;
			return DPSTAT_CHANGEDP;
		}
	}
	if (!pcm_call_is_valid(&datapump->call))
		goto failure;
	*exact_output = true;
	return connected ? DPSTAT_CONNECT : DPSTAT_OK;

failure:
	(void)training_session_fail(&datapump->training);
	mute_output(law, transmit_codewords, transmit_samples, count);
	return DPSTAT_ERROR;
}

int v90_datapump_register(void)
{
	if (modem_dp_register(DP_V90, &v90_operations) != 0)
		return -1;
	if (modem_dp_register(DP_V92, &v90_operations) == 0)
		return 0;
	(void)modem_dp_deregister(DP_V90, &v90_operations);
	return -1;
}

int v90_datapump_unregister(void)
{
	if (v90_operations.use_count != 0 ||
	    modem_dp_get_registered_ops(DP_V90) != &v90_operations ||
	    modem_dp_get_registered_ops(DP_V92) != &v90_operations ||
	    modem_dp_deregister(DP_V92, &v90_operations) != 0)
		return -1;
	if (modem_dp_deregister(DP_V90, &v90_operations) == 0)
		return 0;
	(void)modem_dp_register(DP_V92, &v90_operations);
	return -1;
}

bool v90_datapump_is_busy(void)
{
	return v90_operations.use_count != 0;
}
