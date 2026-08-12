// SPDX-License-Identifier: BSD-3-Clause

#include "fsk300_datapump.h"

#include "modem.h"
#include "modem_param.h"
#include "fsk.h"
#include "v21.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
	/* The tone-pair detector already rejects weak and ambiguous energy.
	 * More than four detector windows are enough to answer the short mark
	 * presented by interoperating V.21 modems without reacting to a transient. */
	FSK300_MARK_ACQUIRE_SAMPLES = MODEM_RATE * 15 / 1000,
	FSK300_GUARD_SAMPLES = MODEM_RATE * 160 / 1000,
	FSK300_ORIGINATE_MARK_SAMPLES = 6400,
	FSK300_CARRIER_LOSS_SAMPLES = 1600,
	FSK300_MIN_LINE_POWER = 10000000,
	FSK300_MIN_DOMINANCE_Q8 = 180,
	BELL103_ORIGINATOR_MARK_HZ = 1270,
	BELL103_ORIGINATOR_SPACE_HZ = 1070,
	BELL103_ANSWERER_MARK_HZ = 2225,
	BELL103_ANSWERER_SPACE_HZ = 2025,
	FSK300_TRANSMIT_AMPLITUDE = 8192,
	FSK300_METRIC_SMOOTHING_SHIFT = 2,
	FSK300_DETECTOR_WINDOW =
		(MODEM_RATE + FSK300_BIT_RATE / 2) /
		FSK300_BIT_RATE,
};

/* Bell 103 assigns the low-frequency pair to the originating station and
 * the high-frequency pair to the answering station. */
static const struct fsk_channel bell103_originator_channel = {
	.sample_rate = MODEM_RATE,
	.symbol_rate_numerator = FSK300_BIT_RATE,
	.symbol_rate_denominator = 1,
	.mark_frequency_hz = BELL103_ORIGINATOR_MARK_HZ,
	.space_frequency_hz = BELL103_ORIGINATOR_SPACE_HZ,
	.detector_window = FSK300_DETECTOR_WINDOW,
	.metric_smoothing_shift = FSK300_METRIC_SMOOTHING_SHIFT,
};

static const struct fsk_channel bell103_answerer_channel = {
	.sample_rate = MODEM_RATE,
	.symbol_rate_numerator = FSK300_BIT_RATE,
	.symbol_rate_denominator = 1,
	.mark_frequency_hz = BELL103_ANSWERER_MARK_HZ,
	.space_frequency_hz = BELL103_ANSWERER_SPACE_HZ,
	.detector_window = FSK300_DETECTOR_WINDOW,
	.metric_smoothing_shift = FSK300_METRIC_SMOOTHING_SHIFT,
};

struct fsk300_profile {
	enum DP_ID id;
	const struct fsk_channel *originator_channel;
	const struct fsk_channel *answerer_channel;
};

static const struct fsk300_profile fsk300_profiles[] = {
	{
		.id = DP_V21,
		.originator_channel = &v21_channel_1,
		.answerer_channel = &v21_channel_2,
	},
	{
		.id = DP_B103,
		.originator_channel = &bell103_originator_channel,
		.answerer_channel = &bell103_answerer_channel,
	},
};

_Static_assert(MODEM_RATE == V21_SAMPLE_RATE,
	       "the V.21 datapump requires 8 kHz audio");
_Static_assert((int)FSK300_BIT_RATE ==
		       (int)V21_BIT_RATE,
	       "the V.21 datapump requires 300 bit/s profiles");

enum fsk300_training_state {
	FSK300_TRAINING_WAIT_MARK,
	FSK300_TRAINING_GUARD,
	FSK300_TRAINING_SEND_MARK,
	FSK300_TRAINING_ONLINE,
};

struct fsk300_datapump {
	struct dp dp;
	const struct fsk300_profile *profile;
	struct fsk_transmitter transmitter;
	struct fsk_receiver receiver;
	enum fsk300_training_state training_state;
	uint64_t training_samples;
	uint64_t training_timeout_samples;
	uint64_t training_state_samples;
	uint32_t remote_mark_samples;
	uint32_t missing_carrier_samples;
	uint8_t transmit_bit;
	bool transmit_bit_pending;
	bool originator;
};

static struct dp *create_fsk300(
	struct modem *modem, enum DP_ID id, int caller, int sample_rate,
	int max_fragment, struct dp_operations *operations);
static int delete_fsk300(struct dp *dp);
static int process_fsk300(
	struct dp *dp, void *receive, void *transmit, int count);

static struct dp_operations fsk300_operations = {
	.name = "300-bit/s FSK",
	.create = create_fsk300,
	.delete = delete_fsk300,
	.process = process_fsk300,
};

static const struct fsk300_profile *find_profile(enum DP_ID id)
{
	for (size_t index = 0;
	     index < sizeof(fsk300_profiles) / sizeof(fsk300_profiles[0]);
	     ++index)
		if (fsk300_profiles[index].id == id)
			return &fsk300_profiles[index];
	return NULL;
}

static struct fsk300_datapump *fsk300_from_dp(struct dp *dp)
{
	struct fsk300_datapump *datapump;

	if (dp == NULL || dp->op != &fsk300_operations ||
	    dp->modem == NULL || dp->dp_data == NULL)
		return NULL;
	datapump = dp->dp_data;
	if (&datapump->dp != dp || datapump->profile == NULL ||
	    datapump->profile->id != dp->id)
		return NULL;
	return datapump;
}

static uint64_t training_timeout(struct modem *modem)
{
	int seconds = modem_get_sreg(modem, SREG_WAIT_CARRIER_AFTER_DIAL);

	if (seconds <= 0)
		seconds = 60;
	return (uint64_t)(unsigned int)seconds * MODEM_RATE;
}

static struct dp *create_fsk300(
	struct modem *modem, enum DP_ID id, int caller, int sample_rate,
	int max_fragment, struct dp_operations *operations)
{
	struct fsk300_datapump *datapump;
	const struct fsk300_profile *profile = find_profile(id);
	const struct fsk_channel *transmit_channel;
	const struct fsk_channel *receive_channel;

	if (modem == NULL || profile == NULL ||
	    (caller != 0 && caller != 1) || sample_rate != MODEM_RATE ||
	    max_fragment != MODEM_FRAG || operations != &fsk300_operations ||
	    operations->use_count == INT_MAX)
		return NULL;
	datapump = calloc(1, sizeof(*datapump));
	if (datapump == NULL)
		return NULL;
	datapump->profile = profile;
	datapump->originator = caller != 0;
	transmit_channel = datapump->originator ? profile->originator_channel :
						 profile->answerer_channel;
	receive_channel = datapump->originator ? profile->answerer_channel :
						profile->originator_channel;
	if (fsk_transmitter_init(
		    &datapump->transmitter, transmit_channel,
		    FSK300_TRANSMIT_AMPLITUDE) !=
		    FSK_OK ||
	    fsk_receiver_init(&datapump->receiver, receive_channel) !=
		    FSK_OK) {
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
	datapump->training_state = FSK300_TRAINING_WAIT_MARK;
	datapump->training_timeout_samples = training_timeout(modem);
	operations->use_count++;
	return &datapump->dp;
}

static int delete_fsk300(struct dp *dp)
{
	struct fsk300_datapump *datapump = fsk300_from_dp(dp);

	if (datapump == NULL || fsk300_operations.use_count <= 0)
		return -1;
	fsk300_operations.use_count--;
	datapump->dp.dp_data = NULL;
	free(datapump);
	return 0;
}

static bool mark_is_present(
	const struct fsk_measurement *measurement)
{
	return measurement->ready &&
	       measurement->line_power >= FSK300_MIN_LINE_POWER &&
	       measurement->dominance_q8 >= FSK300_MIN_DOMINANCE_Q8 &&
	       measurement->mark_power > measurement->space_power;
}

static bool carrier_is_present(
	const struct fsk_measurement *measurement)
{
	return measurement->ready &&
	       measurement->line_power >= FSK300_MIN_LINE_POWER &&
	       measurement->dominance_q8 >= FSK300_MIN_DOMINANCE_Q8;
}

static void generate_mark(
	struct fsk300_datapump *datapump,
	int16_t *transmit,
	int count)
{
	for (int index = 0; index < count; ++index) {
		bool consumed;

		(void)fsk_transmitter_sample(
			&datapump->transmitter, 1, &transmit[index], &consumed);
	}
}

static void generate_data(
	struct fsk300_datapump *datapump,
	int16_t *transmit,
	int count)
{
	for (int index = 0; index < count; ++index) {
		bool consumed;

		if (!datapump->transmit_bit_pending &&
		    fsk_transmitter_can_accept_bit(
			    &datapump->transmitter)) {
			datapump->transmit_bit = 1;
			if (modem_get_bits(
				    datapump->dp.modem, 1,
				    &datapump->transmit_bit, 1) != 1)
				datapump->transmit_bit = 1;
			datapump->transmit_bit &= 1u;
			datapump->transmit_bit_pending = true;
		}
		(void)fsk_transmitter_sample(
			&datapump->transmitter,
			datapump->transmit_bit_pending ?
				datapump->transmit_bit : 1,
			&transmit[index], &consumed);
		if (consumed)
			datapump->transmit_bit_pending = false;
	}
}

static void receive_samples(
	struct fsk300_datapump *datapump,
	const int16_t *receive,
	int count,
	bool deliver_bits)
{
	for (int index = 0; index < count; ++index) {
		uint8_t bit;
		bool ready;

		(void)fsk_receiver_sample(
			&datapump->receiver, receive[index], &bit, &ready);
		if (deliver_bits && ready)
			(void)modem_put_bits(datapump->dp.modem, 1, &bit, 1);
	}
}

static int advance_training(
	struct fsk300_datapump *datapump,
	const struct fsk_measurement *measurement,
	unsigned int count)
{
	if (datapump->training_samples + count >
	    datapump->training_timeout_samples)
		return DPSTAT_ERROR;
	datapump->training_samples += count;

	if (datapump->training_state == FSK300_TRAINING_WAIT_MARK) {
		if (mark_is_present(measurement)) {
			datapump->remote_mark_samples += count;
		} else {
			datapump->remote_mark_samples = 0;
		}
		if (datapump->remote_mark_samples < FSK300_MARK_ACQUIRE_SAMPLES)
			return DPSTAT_OK;
		datapump->training_state = datapump->originator ?
			FSK300_TRAINING_GUARD : FSK300_TRAINING_SEND_MARK;
		datapump->training_state_samples = 0;
		return DPSTAT_OK;
	}
	if (datapump->training_state == FSK300_TRAINING_GUARD) {
		datapump->training_state_samples += count;
		if (datapump->training_state_samples < FSK300_GUARD_SAMPLES)
			return DPSTAT_OK;
		datapump->training_state = FSK300_TRAINING_SEND_MARK;
		datapump->training_state_samples = 0;
		return DPSTAT_OK;
	}
	if (datapump->training_state == FSK300_TRAINING_SEND_MARK) {
		uint64_t required = datapump->originator ?
			FSK300_ORIGINATE_MARK_SAMPLES : FSK300_GUARD_SAMPLES;

		datapump->training_state_samples += count;
		if (datapump->training_state_samples < required)
			return DPSTAT_OK;
		if (modem_set_param(
			    datapump->dp.modem, MDMPRM_TX_RATE,
			    FSK300_BIT_RATE) != 0 ||
		    modem_set_param(
			    datapump->dp.modem, MDMPRM_RX_RATE,
			    FSK300_BIT_RATE) != 0)
			return DPSTAT_ERROR;
		datapump->training_state = FSK300_TRAINING_ONLINE;
		datapump->missing_carrier_samples = 0;
		datapump->dp.status = DPSTAT_CONNECT;
		return DPSTAT_CONNECT;
	}
	return DPSTAT_OK;
}

static int process_fsk300(struct dp *dp, void *receive_buffer,
		       void *transmit_buffer, int count)
{
	struct fsk300_datapump *datapump = fsk300_from_dp(dp);
	const int16_t *receive = receive_buffer;
	int16_t *transmit = transmit_buffer;
	struct fsk_measurement measurement;

	if (datapump == NULL || receive == NULL || transmit == NULL || count <= 0 ||
	    count > MODEM_FRAG) {
		if (transmit != NULL && count > 0 && count <= MODEM_FRAG)
			memset(transmit, 0, (size_t)count * sizeof(*transmit));
		return DPSTAT_ERROR;
	}
	if ((datapump->training_state == FSK300_TRAINING_WAIT_MARK ||
	     datapump->training_state == FSK300_TRAINING_GUARD) &&
	    datapump->originator) {
		memset(transmit, 0, (size_t)count * sizeof(*transmit));
	} else if (datapump->training_state == FSK300_TRAINING_ONLINE) {
		generate_data(datapump, transmit, count);
	} else {
		generate_mark(datapump, transmit, count);
	}
	receive_samples(
		datapump, receive, count,
		datapump->training_state == FSK300_TRAINING_ONLINE);
	measurement = fsk_receiver_measure(&datapump->receiver);

	if (datapump->training_state != FSK300_TRAINING_ONLINE)
		return advance_training(datapump, &measurement, (unsigned int)count);
	if (carrier_is_present(&measurement)) {
		datapump->missing_carrier_samples = 0;
	} else if (datapump->missing_carrier_samples <
		   FSK300_CARRIER_LOSS_SAMPLES) {
		datapump->missing_carrier_samples += (unsigned int)count;
	}
	return datapump->missing_carrier_samples >= FSK300_CARRIER_LOSS_SAMPLES ?
		       DPSTAT_ERROR : DPSTAT_OK;
}

int fsk300_datapump_register(void)
{
	if (modem_dp_register(DP_V21, &fsk300_operations) != 0)
		return -1;
	if (modem_dp_register(DP_B103, &fsk300_operations) == 0)
		return 0;
	(void)modem_dp_deregister(DP_V21, &fsk300_operations);
	return -1;
}

int fsk300_datapump_unregister(void)
{
	if (fsk300_operations.use_count != 0 ||
	    modem_dp_get_registered_ops(DP_V21) != &fsk300_operations ||
	    modem_dp_get_registered_ops(DP_B103) != &fsk300_operations ||
	    modem_dp_deregister(DP_B103, &fsk300_operations) != 0)
		return -1;
	if (modem_dp_deregister(DP_V21, &fsk300_operations) == 0)
		return 0;
	(void)modem_dp_register(DP_B103, &fsk300_operations);
	return -1;
}

bool fsk300_datapump_is_busy(void)
{
	return fsk300_operations.use_count != 0;
}
