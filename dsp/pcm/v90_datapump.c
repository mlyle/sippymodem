// SPDX-License-Identifier: BSD-3-Clause

#include "v90_datapump.h"

#include "datapump_timing.h"
#include "modem.h"
#include "modem_param.h"
#include "pcm_call.h"
#include "training_session.h"
#include "v90_profile.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct v90_datapump {
	struct dp dp;
	struct pcm_call call;
	struct training_session training;
	enum v90_modem_role role;
	enum g711_law media_law;
	bool call_started;
};

/* The integrated digital network currently uses the North American PCM
 * codebook.  SIP companding is an independent transport choice. */
static const enum g711_law v90_pcm_law = G711_LAW_PCMU;

static struct dp *create_v90(
	struct modem *modem, enum DP_ID id, int caller, int sample_rate,
	int max_fragment, struct dp_operations *operations);
static int delete_v90(struct dp *dp);
static int process_v90(
	struct dp *dp, void *receive, void *transmit, int count);
static int process_v90_g711(
	struct dp *dp, enum g711_law law,
	const uint8_t *receive_codewords, void *receive_samples,
	uint8_t *transmit_codewords, void *transmit_samples,
	int count, bool *exact_output);
static int retrain_v90(struct dp *dp);

static struct dp_operations v90_operations = {
	.name = "V.90",
	.create = create_v90,
	.delete = delete_v90,
	.process = process_v90,
	.process_g711 = process_v90_g711,
	.retrain = retrain_v90,
};

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
	enum v90_modem_role role;

	if (dp == NULL || dp->id != DP_V90 || dp->op != &v90_operations ||
	    dp->modem == NULL || dp->dp_data == NULL ||
	    !role_from_modem(dp->modem, &role))
		return NULL;
	datapump = dp->dp_data;
	if (&datapump->dp != dp || datapump->role != role ||
	    !training_session_is_valid(&datapump->training) ||
	    (datapump->call_started &&
	     (!g711_law_is_valid(datapump->media_law) ||
	      !pcm_call_is_valid(&datapump->call) ||
	      datapump->call.local_role != datapump->role ||
	      datapump->call.law != v90_pcm_law ||
	      (pcm_call_has_failed(&datapump->call) &&
	       !training_session_has_failed(&datapump->training)))) ||
	    (!training_session_has_failed(&datapump->training) &&
	     training_session_is_online(&datapump->training) !=
		     (datapump->call_started &&
		      pcm_call_is_online(&datapump->call)) &&
	     !(training_session_is_retraining(&datapump->training) &&
	       !pcm_call_is_online(&datapump->call))) ||
	    (!training_session_has_failed(&datapump->training) &&
	     datapump->call_started && pcm_call_is_online(&datapump->call) &&
	     dp->status != DPSTAT_CONNECT) ||
	    (!training_session_has_failed(&datapump->training) &&
	     (!datapump->call_started || !pcm_call_is_online(&datapump->call)) &&
	     dp->status == DPSTAT_CONNECT &&
	     !training_session_is_retraining(&datapump->training)))
		return NULL;
	return datapump;
}

static struct dp *create_v90(
	struct modem *modem, enum DP_ID id, int caller, int sample_rate,
	int max_fragment, struct dp_operations *operations)
{
	struct v90_datapump *datapump;
	struct pcm_downstream_rate_selection downstream;
	enum v90_modem_role role;

	if (modem == NULL || id != DP_V90 || (caller != 0 && caller != 1) ||
	    sample_rate != MODEM_RATE || max_fragment != MODEM_FRAG ||
	    operations != &v90_operations || operations->use_count == INT_MAX ||
	    !role_from_modem(modem, &role) ||
	    !pcm_downstream_rates_select(
		    &downstream, modem->min_rate, modem->max_rate))
		return NULL;
	datapump = calloc(1, sizeof(*datapump));
	if (datapump == NULL)
		return NULL;
	datapump->role = role;
	if (training_session_init(
		    &datapump->training,
		    datapump_carrier_wait_ticks(modem)) !=
	    TRAINING_SESSION_OK) {
		free(datapump);
		return NULL;
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
	free(datapump);
	return 0;
}

static int retrain_v90(struct dp *dp)
{
	struct v90_datapump *datapump = v90_from_dp(dp);
	struct training_session training;
	struct pcm_call call;

	if (datapump == NULL)
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
	datapump->call = call;
	datapump->training = training;
	return 0;
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

static bool fill_source_bits(
	struct v90_datapump *datapump, uint8_t *bits, size_t bit_count)
{
	int supplied = 0;

	if (datapump == NULL || bits == NULL || bit_count > INT_MAX)
		return false;
	memset(bits, 1, bit_count);
	if (training_session_is_online(&datapump->training) &&
	    datapump->dp.modem->get_bits != NULL)
		supplied = modem_get_bits(
			datapump->dp.modem, 1, bits, (int)bit_count);
	if (supplied < 0)
		supplied = 0;
	if ((size_t)supplied > bit_count)
		supplied = (int)bit_count;
	for (size_t index = 0; index < (size_t)supplied; ++index)
		bits[index] &= 1u;
	return true;
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
	if (!fill_source_bits(datapump, bits, bit_count))
		return false;
	memcpy(primary_bits, bits, request->primary_bits);
	if (auxiliary_bit != NULL)
		*auxiliary_bit = bits[request->primary_bits];
	return true;
}

static bool source_downstream_frame(
	void *context, uint8_t *bits, size_t bit_count)
{
	if (bit_count == 0 || bit_count > V90_PCM_FRAME_MAX_BITS)
		return false;
	return fill_source_bits(context, bits, bit_count);
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
	uint32_t transmit_rate;
	uint32_t receive_rate;

	if (!pcm_call_get_data_rates(
		    &datapump->call, &transmit_rate, &receive_rate) ||
	    transmit_rate > INT_MAX || receive_rate > INT_MAX ||
	    modem_set_param(
		    datapump->dp.modem, MDMPRM_TX_RATE,
		    (int)transmit_rate) != 0 ||
	    modem_set_param(
		    datapump->dp.modem, MDMPRM_RX_RATE,
		    (int)receive_rate) != 0)
		return false;
	if (training_session_connect(&datapump->training) !=
	    TRAINING_SESSION_OK)
		return false;
	datapump->dp.status = DPSTAT_CONNECT;
	return true;
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
	};

	if (datapump->call_started)
		return datapump->media_law == media_law;
	if (v90_profile_init(
		    &configuration, datapump->role, v90_pcm_law,
		    datapump->dp.modem->min_rate,
		    datapump->dp.modem->max_rate, &upstream) != V90_PROFILE_OK ||
	    pcm_call_init(&datapump->call, &configuration) != PCM_CALL_OK)
		return false;
	datapump->media_law = media_law;
	datapump->call_started = true;
	return true;
}

static uint8_t media_to_pcm_octet(enum g711_law media_law, uint8_t octet)
{
	uint8_t pcmu;

	if (media_law == v90_pcm_law)
		return octet;
	/* A-law retains two signed minimum-magnitude characters.  Preserve
	 * their sign as the two PCMU zero characters instead of rounding both
	 * to PCMU magnitude one. */
	if (octet == g711_encode_pcma(0))
		return g711_encode_pcmu(0);
	if (octet == g711_encode_pcma(-1))
		return g711_encode_pcmu(-1);
	pcmu = g711_encode_pcmu(g711_decode_pcma(octet));
	if (g711_encode_pcma(g711_decode_pcmu(pcmu)) == octet)
		return pcmu;
	/* The negative A-law bias puts a small set of exact tandem levels on
	 * the lower neighboring PCMU character.  Recover that character rather
	 * than moving a valid V.90 U-code by one. */
	if (pcmu != 0 &&
	    g711_encode_pcma(g711_decode_pcmu((uint8_t)(pcmu - 1u))) == octet)
		return (uint8_t)(pcmu - 1u);
	return pcmu;
}

static uint8_t pcm_to_media_octet(enum g711_law media_law, uint8_t octet)
{
	if (media_law == v90_pcm_law)
		return octet;
	if (octet == g711_encode_pcmu(-1))
		return g711_encode_pcma(-1);
	return g711_encode_pcma(g711_decode_pcmu(octet));
}

static bool process_octet(
	struct v90_datapump *datapump, uint8_t received,
	uint8_t *transmitted, bool *connected)
{
	struct pcm_call_data_sources sources = {
		.upstream = source_upstream_frame,
		.upstream_context = datapump,
		.downstream = source_downstream_frame,
		.downstream_context = datapump,
	};
	struct pcm_call_data_event event = { 0 };
	enum pcm_call_result result;
	bool was_online = training_session_is_online(&datapump->training);

	/* The datapump is the serialized owner of a call that every public
	 * transition validates.  Consult its stage directly here instead of
	 * recursively validating the complete PCM graph several times per
	 * network character. */
	if (datapump->call.stage == PCM_CALL_PHASE4 &&
	    pcm_call_phase4_is_ready(&datapump->call) &&
	    pcm_call_start_data_path(&datapump->call) != PCM_CALL_OK)
		return false;
	if (datapump->call.stage == PCM_CALL_DATA_PATH) {
		result = pcm_call_data_octet(
			&datapump->call, received, &sources,
			transmitted, &event);
	} else {
		struct pcm_call_observation observation;

		result = pcm_call_octet(
			&datapump->call, received, transmitted, &observation);
	}
	if (result != PCM_CALL_OK)
		return false;
	if (was_online &&
	    datapump->call.stage == PCM_CALL_RETRAIN_SILENCE &&
	    training_session_start_retrain(
		    &datapump->training, TRAINING_REMOTE_RETRAIN) !=
		    TRAINING_SESSION_OK)
		return false;
	if (!training_session_is_online(&datapump->training) &&
	    pcm_call_is_online(&datapump->call)) {
		if (!enter_link(datapump))
			return false;
		*connected = true;
	}
	return deliver_event(datapump, &event);
}

static void mute_output(
	enum g711_law law, uint8_t *codewords,
	int16_t *samples, int count)
{
	uint8_t silence = law == G711_LAW_PCMA ?
				  g711_encode_pcma(0) : g711_encode_pcmu(0);

	if (count <= 0 || count > MODEM_FRAG)
		return;
	if (codewords != NULL)
		memset(codewords, silence, (size_t)count);
	if (samples != NULL)
		memset(samples, 0, (size_t)count * sizeof(*samples));
}

static int process_v90_g711(
	struct dp *dp, enum g711_law law,
	const uint8_t *receive_codewords, void *receive_samples_buffer,
	uint8_t *transmit_codewords, void *transmit_samples_buffer,
	int count, bool *exact_output)
{
	struct v90_datapump *datapump = v90_from_dp(dp);
	const int16_t *receive_samples = receive_samples_buffer;
	int16_t *transmit_samples = transmit_samples_buffer;
	bool connected = false;

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
	for (int index = 0; index < count; ++index) {
		uint8_t pcm_transmit;

		if (!training_session_is_online(&datapump->training) &&
		    training_session_advance(&datapump->training, 1) !=
			    TRAINING_SESSION_OK)
			goto failure;
		if (!process_octet(
			    datapump,
			    media_to_pcm_octet(law, receive_codewords[index]),
			    &pcm_transmit, &connected))
			goto failure;
		transmit_codewords[index] =
			pcm_to_media_octet(law, pcm_transmit);
		transmit_samples[index] = law == G711_LAW_PCMA ?
			g711_decode_pcma(transmit_codewords[index]) :
			g711_decode_pcmu(transmit_codewords[index]);
	}
	*exact_output = true;
	return connected ? DPSTAT_CONNECT : DPSTAT_OK;

failure:
	(void)training_session_fail(&datapump->training);
	mute_output(law, transmit_codewords, transmit_samples, count);
	return DPSTAT_ERROR;
}

int v90_datapump_register(void)
{
	return modem_dp_register(DP_V90, &v90_operations);
}

int v90_datapump_unregister(void)
{
	return modem_dp_deregister(DP_V90, &v90_operations);
}

bool v90_datapump_is_busy(void)
{
	return v90_operations.use_count != 0;
}
