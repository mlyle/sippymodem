// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_v21_datapump.h"

#include "modem.h"
#include "modem_param.h"
#include "sippymodem_v21.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
	V21_MARK_ACQUIRE_SAMPLES = 480,
	V21_ANSWER_GUARD_SAMPLES = 1280,
	V21_ORIGINATE_MARK_SAMPLES = 6400,
	V21_CARRIER_LOSS_SAMPLES = 1600,
	V21_MIN_LINE_POWER = 10000000,
	V21_MIN_DOMINANCE_Q8 = 180,
};

enum sippymodem_v21_training_state {
	V21_TRAINING_WAIT_MARK,
	V21_TRAINING_SEND_MARK,
	V21_TRAINING_ONLINE,
};

struct sippymodem_v21_datapump {
	struct dp dp;
	struct sippymodem_v21_transmitter transmitter;
	struct sippymodem_v21_receiver receiver;
	enum sippymodem_v21_training_state training_state;
	uint64_t training_samples;
	uint64_t training_timeout_samples;
	uint64_t sent_mark_samples;
	uint32_t remote_mark_samples;
	uint32_t missing_carrier_samples;
	uint8_t transmit_bit;
	bool transmit_bit_pending;
	bool originator;
};

static struct dp *create_v21(
	struct modem *modem, enum DP_ID id, int caller, int sample_rate,
	int max_fragment, struct dp_operations *operations);
static int delete_v21(struct dp *dp);
static int process_v21(struct dp *dp, void *receive, void *transmit, int count);

static struct dp_operations v21_operations = {
	.name = "sippymodem V.21",
	.create = create_v21,
	.delete = delete_v21,
	.process = process_v21,
};

static struct sippymodem_v21_datapump *v21_from_dp(struct dp *dp)
{
	struct sippymodem_v21_datapump *datapump;

	if (dp == NULL || dp->id != DP_V21 || dp->op != &v21_operations ||
	    dp->modem == NULL || dp->dp_data == NULL)
		return NULL;
	datapump = dp->dp_data;
	return &datapump->dp == dp ? datapump : NULL;
}

static uint64_t training_timeout(struct modem *modem)
{
	int seconds = modem_get_sreg(modem, SREG_WAIT_CARRIER_AFTER_DIAL);

	if (seconds <= 0)
		seconds = 60;
	return (uint64_t)(unsigned int)seconds * SIPPYMODEM_V21_SAMPLE_RATE;
}

static struct dp *create_v21(
	struct modem *modem, enum DP_ID id, int caller, int sample_rate,
	int max_fragment, struct dp_operations *operations)
{
	struct sippymodem_v21_datapump *datapump;
	enum sippymodem_v21_channel transmit_channel;
	enum sippymodem_v21_channel receive_channel;

	if (modem == NULL || id != DP_V21 ||
	    (caller != 0 && caller != 1) || sample_rate != MODEM_RATE ||
	    max_fragment != MODEM_FRAG || operations != &v21_operations ||
	    operations->use_count == INT_MAX)
		return NULL;
	datapump = calloc(1, sizeof(*datapump));
	if (datapump == NULL)
		return NULL;
	datapump->originator = caller != 0;
	transmit_channel = datapump->originator ? SIPPYMODEM_V21_CHANNEL_1 :
						 SIPPYMODEM_V21_CHANNEL_2;
	receive_channel = datapump->originator ? SIPPYMODEM_V21_CHANNEL_2 :
						SIPPYMODEM_V21_CHANNEL_1;
	if (sippymodem_v21_transmitter_init(
		    &datapump->transmitter, transmit_channel) !=
		    SIPPYMODEM_V21_OK ||
	    sippymodem_v21_receiver_init(&datapump->receiver, receive_channel) !=
		    SIPPYMODEM_V21_OK) {
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
	datapump->training_state = V21_TRAINING_WAIT_MARK;
	datapump->training_timeout_samples = training_timeout(modem);
	operations->use_count++;
	return &datapump->dp;
}

static int delete_v21(struct dp *dp)
{
	struct sippymodem_v21_datapump *datapump = v21_from_dp(dp);

	if (datapump == NULL || v21_operations.use_count <= 0)
		return -1;
	v21_operations.use_count--;
	datapump->dp.dp_data = NULL;
	free(datapump);
	return 0;
}

static bool mark_is_present(
	const struct sippymodem_v21_measurement *measurement)
{
	return measurement->ready &&
	       measurement->line_power >= V21_MIN_LINE_POWER &&
	       measurement->dominance_q8 >= V21_MIN_DOMINANCE_Q8 &&
	       measurement->mark_power > measurement->space_power;
}

static bool carrier_is_present(
	const struct sippymodem_v21_measurement *measurement)
{
	return measurement->ready &&
	       measurement->line_power >= V21_MIN_LINE_POWER &&
	       measurement->dominance_q8 >= V21_MIN_DOMINANCE_Q8;
}

static void generate_mark(
	struct sippymodem_v21_datapump *datapump,
	int16_t *transmit,
	int count)
{
	for (int index = 0; index < count; ++index) {
		bool consumed;

		(void)sippymodem_v21_transmitter_sample(
			&datapump->transmitter, 1, &transmit[index], &consumed);
	}
}

static void generate_data(
	struct sippymodem_v21_datapump *datapump,
	int16_t *transmit,
	int count)
{
	for (int index = 0; index < count; ++index) {
		bool consumed;

		if (!datapump->transmit_bit_pending &&
		    !datapump->transmitter.holding_bit) {
			datapump->transmit_bit = 1;
			if (modem_get_bits(
				    datapump->dp.modem, 1,
				    &datapump->transmit_bit, 1) != 1)
				datapump->transmit_bit = 1;
			datapump->transmit_bit &= 1u;
			datapump->transmit_bit_pending = true;
		}
		(void)sippymodem_v21_transmitter_sample(
			&datapump->transmitter,
			datapump->transmit_bit_pending ?
				datapump->transmit_bit : 1,
			&transmit[index], &consumed);
		if (consumed)
			datapump->transmit_bit_pending = false;
	}
}

static void receive_samples(
	struct sippymodem_v21_datapump *datapump,
	const int16_t *receive,
	int count,
	bool deliver_bits)
{
	for (int index = 0; index < count; ++index) {
		uint8_t bit;
		bool ready;

		(void)sippymodem_v21_receiver_sample(
			&datapump->receiver, receive[index], &bit, &ready);
		if (deliver_bits && ready)
			(void)modem_put_bits(datapump->dp.modem, 1, &bit, 1);
	}
}

static int advance_training(
	struct sippymodem_v21_datapump *datapump,
	const struct sippymodem_v21_measurement *measurement,
	unsigned int count)
{
	if (datapump->training_samples + count >
	    datapump->training_timeout_samples)
		return DPSTAT_ERROR;
	datapump->training_samples += count;

	if (datapump->training_state == V21_TRAINING_WAIT_MARK) {
		if (mark_is_present(measurement)) {
			datapump->remote_mark_samples += count;
		} else {
			datapump->remote_mark_samples = 0;
		}
		if (datapump->remote_mark_samples < V21_MARK_ACQUIRE_SAMPLES)
			return DPSTAT_OK;
		datapump->training_state = V21_TRAINING_SEND_MARK;
		datapump->sent_mark_samples = 0;
		return DPSTAT_OK;
	}
	if (datapump->training_state == V21_TRAINING_SEND_MARK) {
		uint64_t required = datapump->originator ?
			V21_ORIGINATE_MARK_SAMPLES : V21_ANSWER_GUARD_SAMPLES;

		datapump->sent_mark_samples += count;
		if (datapump->sent_mark_samples < required)
			return DPSTAT_OK;
		if (modem_set_param(
			    datapump->dp.modem, MDMPRM_TX_RATE,
			    SIPPYMODEM_V21_BAUD) != 0 ||
		    modem_set_param(
			    datapump->dp.modem, MDMPRM_RX_RATE,
			    SIPPYMODEM_V21_BAUD) != 0)
			return DPSTAT_ERROR;
		datapump->training_state = V21_TRAINING_ONLINE;
		datapump->missing_carrier_samples = 0;
		datapump->dp.status = DPSTAT_CONNECT;
		return DPSTAT_CONNECT;
	}
	return DPSTAT_OK;
}

static int process_v21(struct dp *dp, void *receive_buffer,
		       void *transmit_buffer, int count)
{
	struct sippymodem_v21_datapump *datapump = v21_from_dp(dp);
	const int16_t *receive = receive_buffer;
	int16_t *transmit = transmit_buffer;
	struct sippymodem_v21_measurement measurement;

	if (datapump == NULL || receive == NULL || transmit == NULL || count <= 0 ||
	    count > MODEM_FRAG) {
		if (transmit != NULL && count > 0 && count <= MODEM_FRAG)
			memset(transmit, 0, (size_t)count * sizeof(*transmit));
		return DPSTAT_ERROR;
	}
	if (datapump->training_state == V21_TRAINING_WAIT_MARK &&
	    datapump->originator) {
		memset(transmit, 0, (size_t)count * sizeof(*transmit));
	} else if (datapump->training_state == V21_TRAINING_ONLINE) {
		generate_data(datapump, transmit, count);
	} else {
		generate_mark(datapump, transmit, count);
	}
	receive_samples(
		datapump, receive, count,
		datapump->training_state == V21_TRAINING_ONLINE);
	measurement = sippymodem_v21_receiver_measure(&datapump->receiver);

	if (datapump->training_state != V21_TRAINING_ONLINE)
		return advance_training(datapump, &measurement, (unsigned int)count);
	if (carrier_is_present(&measurement)) {
		datapump->missing_carrier_samples = 0;
	} else if (datapump->missing_carrier_samples <
		   V21_CARRIER_LOSS_SAMPLES) {
		datapump->missing_carrier_samples += (unsigned int)count;
	}
	return datapump->missing_carrier_samples >= V21_CARRIER_LOSS_SAMPLES ?
		       DPSTAT_ERROR : DPSTAT_OK;
}

int sippymodem_v21_datapump_register(void)
{
	return modem_dp_register(DP_V21, &v21_operations);
}

int sippymodem_v21_datapump_unregister(void)
{
	return modem_dp_deregister(DP_V21, &v21_operations);
}

bool sippymodem_v21_datapump_is_busy(void)
{
	return v21_operations.use_count != 0;
}
