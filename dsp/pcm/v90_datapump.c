// SPDX-License-Identifier: BSD-3-Clause

#include "v90_datapump.h"

#include "datapump_timing.h"
#include "modem.h"
#include "modem_param.h"
#include "v90_call.h"
#include "v90_profile.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct v90_datapump {
	struct dp dp;
	struct v90_call call;
	uint64_t training_octets;
	uint64_t training_timeout_octets;
	enum v90_modem_role role;
	enum g711_law law;
	bool call_started;
	bool linked;
	bool failed;
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
	uint8_t *transmit_codewords, void *transmit_samples,
	int count, bool *exact_output);

static struct dp_operations v90_operations = {
	.name = "V.90",
	.create = create_v90,
	.delete = delete_v90,
	.process = process_v90,
	.process_g711 = process_v90_g711,
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
	    datapump->training_timeout_octets == 0 ||
	    datapump->training_octets > datapump->training_timeout_octets ||
	    (datapump->call_started &&
	     (!g711_law_is_valid(datapump->law) ||
	      !v90_call_is_valid(&datapump->call) ||
	      datapump->call.local_role != datapump->role ||
	      datapump->call.law != datapump->law ||
	      (v90_call_has_failed(&datapump->call) && !datapump->failed))) ||
	    (!datapump->call_started && (datapump->linked || datapump->failed)) ||
	    (!datapump->failed && datapump->linked !=
				       v90_call_is_online(&datapump->call)) ||
	    (datapump->linked && dp->status != DPSTAT_CONNECT) ||
	    (!datapump->linked && dp->status == DPSTAT_CONNECT))
		return NULL;
	return datapump;
}

static struct dp *create_v90(
	struct modem *modem, enum DP_ID id, int caller, int sample_rate,
	int max_fragment, struct dp_operations *operations)
{
	struct v90_datapump *datapump;
	enum v90_modem_role role;

	if (modem == NULL || id != DP_V90 || (caller != 0 && caller != 1) ||
	    sample_rate != MODEM_RATE || max_fragment != MODEM_FRAG ||
	    operations != &v90_operations || operations->use_count == INT_MAX ||
	    !role_from_modem(modem, &role) ||
	    modem->min_rate > V90_PROFILE_DOWNSTREAM_BIT_RATE ||
	    modem->max_rate < V90_PROFILE_DOWNSTREAM_BIT_RATE)
		return NULL;
	datapump = calloc(1, sizeof(*datapump));
	if (datapump == NULL)
		return NULL;
	datapump->role = role;
	datapump->training_timeout_octets =
		datapump_carrier_wait_ticks(modem);
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
	if (datapump->linked && datapump->dp.modem->get_bits != NULL)
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
	if (!datapump->linked || datapump->dp.modem->put_bits == NULL)
		return true;
	return modem_put_bits(
		       datapump->dp.modem, 1, copied, (int)bit_count) ==
	       (int)bit_count;
}

static bool deliver_event(
	struct v90_datapump *datapump,
	const struct v90_call_data_event *event)
{
	uint8_t bits[V34_MAX_MAPPING_FRAME_BITS + 1];
	size_t bit_count;

	if (event->kind == V90_CALL_DATA_NONE)
		return true;
	if (event->kind == V90_CALL_DOWNSTREAM_FRAME) {
		if (datapump->role != V90_ANALOG_MODEM)
			return false;
		return deliver_bits(
			datapump, event->frame.downstream.bits,
			event->frame.downstream.bit_count);
	}
	if (event->kind != V90_CALL_UPSTREAM_FRAME ||
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

	if (!v90_call_get_data_rates(
		    &datapump->call, &transmit_rate, &receive_rate) ||
	    transmit_rate > INT_MAX || receive_rate > INT_MAX ||
	    modem_set_param(
		    datapump->dp.modem, MDMPRM_TX_RATE,
		    (int)transmit_rate) != 0 ||
	    modem_set_param(
		    datapump->dp.modem, MDMPRM_RX_RATE,
		    (int)receive_rate) != 0)
		return false;
	datapump->linked = true;
	datapump->dp.status = DPSTAT_CONNECT;
	return true;
}

static bool start_call(
	struct v90_datapump *datapump, enum g711_law law)
{
	struct v90_call_configuration configuration;

	if (datapump->call_started)
		return datapump->law == law;
	if (v90_profile_init(
		    &configuration, datapump->role, law,
		    datapump->dp.modem->min_rate,
		    datapump->dp.modem->max_rate) != V90_PROFILE_OK ||
	    v90_call_init(&datapump->call, &configuration) != V90_CALL_OK)
		return false;
	datapump->law = law;
	datapump->call_started = true;
	return true;
}

static bool process_octet(
	struct v90_datapump *datapump, uint8_t received,
	uint8_t *transmitted, bool *connected)
{
	struct v90_call_data_sources sources = {
		.upstream = source_upstream_frame,
		.upstream_context = datapump,
		.downstream = source_downstream_frame,
		.downstream_context = datapump,
	};
	struct v90_call_data_event event = { 0 };
	enum v90_call_result result;

	if (v90_call_phase4_is_ready(&datapump->call) &&
	    v90_call_start_data_path(&datapump->call) != V90_CALL_OK)
		return false;
	if (v90_call_current_stage(&datapump->call) == V90_CALL_DATA_PATH) {
		result = v90_call_data_octet(
			&datapump->call, received, &sources,
			transmitted, &event);
	} else {
		struct v90_call_observation observation;

		result = v90_call_octet(
			&datapump->call, received, transmitted, &observation);
	}
	if (result != V90_CALL_OK)
		return false;
	if (!datapump->linked && v90_call_is_online(&datapump->call)) {
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
	    exact_output == NULL || count <= 0 || count > MODEM_FRAG ||
	    datapump->failed || !start_call(datapump, law)) {
		mute_output(law, transmit_codewords, transmit_samples, count);
		return DPSTAT_ERROR;
	}
	for (int index = 0; index < count; ++index) {
		if (!datapump->linked) {
			if (datapump->training_octets >=
			    datapump->training_timeout_octets)
				goto failure;
			datapump->training_octets++;
		}
		if (!process_octet(
			    datapump, receive_codewords[index],
			    &transmit_codewords[index], &connected))
			goto failure;
		transmit_samples[index] = law == G711_LAW_PCMA ?
			g711_decode_pcma(transmit_codewords[index]) :
			g711_decode_pcmu(transmit_codewords[index]);
	}
	*exact_output = true;
	return connected ? DPSTAT_CONNECT : DPSTAT_OK;

failure:
	datapump->failed = true;
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
