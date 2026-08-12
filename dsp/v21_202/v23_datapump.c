// SPDX-License-Identifier: BSD-3-Clause

#include "v23_datapump.h"

#include "modem.h"
#include "modem_param.h"
#include "fsk.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
	V23_FORWARD_MARK_HZ = 1300,
	V23_FORWARD_SPACE_HZ = 2100,
	V23_BACKWARD_MARK_HZ = 390,
	V23_BACKWARD_SPACE_HZ = 450,
	V23_FORWARD_DETECTOR_WINDOW = 11,
	V23_BACKWARD_DETECTOR_WINDOW = 128,
	V23_METRIC_SMOOTHING_SHIFT = 1,
	V23_TRANSMIT_AMPLITUDE = 8000,
	V23_ANSWER_TONE_SAMPLES = MODEM_RATE * 3,
	V23_ANSWER_SILENCE_SAMPLES = MODEM_RATE / 20,
	V23_FORWARD_MARK_ACQUIRE_SAMPLES = MODEM_RATE / 25,
	V23_BACKWARD_MARK_ACQUIRE_SAMPLES = MODEM_RATE / 5,
	V23_PRESENTATION_MIN_SAMPLES = MODEM_RATE * 3 / 10,
	V23_PRESENTATION_MAX_SAMPLES = MODEM_RATE * 6 / 10,
	V23_CARRIER_LOSS_SAMPLES = MODEM_RATE * 7 / 10,
	V23_MIN_LINE_POWER = 10000000,
	V23_MIN_DOMINANCE_Q8 = 180,
	V23_PRELINK_BIT_CAPACITY = 1024,
};

_Static_assert(
	(V23_PRESENTATION_MAX_SAMPLES + V23_FORWARD_DETECTOR_WINDOW) *
			V23_FORWARD_BIT_RATE / MODEM_RATE <
		V23_PRELINK_BIT_CAPACITY,
	"V.23 pre-link storage must cover the maximum presentation interval");

static const struct fsk_channel v23_forward_channel = {
	.sample_rate = MODEM_RATE,
	.symbol_rate_numerator = V23_FORWARD_BIT_RATE,
	.symbol_rate_denominator = 1,
	.mark_frequency_hz = V23_FORWARD_MARK_HZ,
	.space_frequency_hz = V23_FORWARD_SPACE_HZ,
	.detector_window = V23_FORWARD_DETECTOR_WINDOW,
	.metric_smoothing_shift = V23_METRIC_SMOOTHING_SHIFT,
};

static const struct fsk_channel v23_backward_channel = {
	.sample_rate = MODEM_RATE,
	.symbol_rate_numerator = V23_BACKWARD_BIT_RATE,
	.symbol_rate_denominator = 1,
	.mark_frequency_hz = V23_BACKWARD_MARK_HZ,
	.space_frequency_hz = V23_BACKWARD_SPACE_HZ,
	.detector_window = V23_BACKWARD_DETECTOR_WINDOW,
	.metric_smoothing_shift = V23_METRIC_SMOOTHING_SHIFT,
};

enum v23_phase {
	V23_PHASE_ANSWER_TONE,
	V23_PHASE_ANSWER_SILENCE,
	V23_PHASE_WAIT_MARK,
	V23_PHASE_PRESENTATION,
	V23_PHASE_ONLINE,
};

struct v23_datapump {
	struct dp dp;
	struct fsk_transmitter transmitter;
	struct fsk_receiver receiver;
	enum v23_phase phase;
	uint64_t training_samples;
	uint64_t training_timeout_samples;
	uint32_t phase_samples;
	uint32_t acquisition_samples;
	uint32_t presentation_samples;
	uint32_t presentation_limit_samples;
	uint32_t missing_carrier_samples;
	uint8_t prelink_bits[V23_PRELINK_BIT_CAPACITY];
	size_t prelink_count;
	uint8_t transmit_bit;
	bool transmit_bit_pending;
	bool answerer;
};

static struct dp *create_v23(
	struct modem *modem, enum DP_ID id, int caller, int sample_rate,
	int max_fragment, struct dp_operations *operations);
static int delete_v23(struct dp *dp);
static int process_v23(
	struct dp *dp, void *receive, void *transmit, int count);

static struct dp_operations v23_operations = {
	.name = "V.23",
	.create = create_v23,
	.delete = delete_v23,
	.process = process_v23,
};

static struct v23_datapump *v23_from_dp(struct dp *dp)
{
	struct v23_datapump *datapump;

	if (dp == NULL || dp->id != DP_V23 || dp->op != &v23_operations ||
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
	return (uint64_t)(unsigned int)seconds * MODEM_RATE;
}

static uint32_t presentation_limit(struct modem *modem)
{
	int deciseconds =
		modem_get_sreg(modem, SREG_CARRIER_DETECT_RESPONSE_TIME);
	uint32_t samples;

	if (deciseconds <= 0)
		return V23_PRESENTATION_MIN_SAMPLES;
	samples = (uint32_t)(unsigned int)deciseconds * (MODEM_RATE / 10);
	if (samples < V23_PRESENTATION_MIN_SAMPLES)
		return V23_PRESENTATION_MIN_SAMPLES;
	if (samples > V23_PRESENTATION_MAX_SAMPLES)
		return V23_PRESENTATION_MAX_SAMPLES;
	return samples;
}

static struct dp *create_v23(
	struct modem *modem, enum DP_ID id, int caller, int sample_rate,
	int max_fragment, struct dp_operations *operations)
{
	struct v23_datapump *datapump;
	const struct fsk_channel *transmit_channel;
	const struct fsk_channel *receive_channel;

	if (modem == NULL || id != DP_V23 || (caller != 0 && caller != 1) ||
	    sample_rate != MODEM_RATE || max_fragment != MODEM_FRAG ||
	    operations != &v23_operations || operations->use_count == INT_MAX)
		return NULL;
	datapump = calloc(1, sizeof(*datapump));
	if (datapump == NULL)
		return NULL;
	datapump->answerer = caller == 0;
	transmit_channel = datapump->answerer ? &v23_forward_channel :
						 &v23_backward_channel;
	receive_channel = datapump->answerer ? &v23_backward_channel :
						&v23_forward_channel;
	if (fsk_transmitter_init(
		    &datapump->transmitter, transmit_channel,
		    V23_TRANSMIT_AMPLITUDE) != FSK_OK ||
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
	datapump->phase = datapump->answerer ? V23_PHASE_ANSWER_TONE :
						 V23_PHASE_WAIT_MARK;
	datapump->training_timeout_samples = training_timeout(modem);
	datapump->presentation_limit_samples = presentation_limit(modem);
	operations->use_count++;
	return &datapump->dp;
}

static int delete_v23(struct dp *dp)
{
	struct v23_datapump *datapump = v23_from_dp(dp);

	if (datapump == NULL || v23_operations.use_count <= 0)
		return -1;
	v23_operations.use_count--;
	datapump->dp.dp_data = NULL;
	free(datapump);
	return 0;
}

static bool phase_receives(enum v23_phase phase)
{
	return phase == V23_PHASE_WAIT_MARK ||
	       phase == V23_PHASE_PRESENTATION || phase == V23_PHASE_ONLINE;
}

static enum fsk_result generate_fsk_sample(
	struct v23_datapump *datapump, uint8_t bit,
	int16_t *sample)
{
	bool consumed;

	return fsk_transmitter_sample(
		&datapump->transmitter, bit, sample, &consumed);
}

static enum fsk_result generate_data_sample(
	struct v23_datapump *datapump, int16_t *sample)
{
	bool consumed;

	if (!datapump->transmit_bit_pending &&
	    fsk_transmitter_can_accept_bit(
		    &datapump->transmitter)) {
		datapump->transmit_bit = 1;
		if (modem_get_bits(
			    datapump->dp.modem, 1, &datapump->transmit_bit, 1) != 1)
			datapump->transmit_bit = 1;
		datapump->transmit_bit &= 1u;
		datapump->transmit_bit_pending = true;
	}
	if (fsk_transmitter_sample(
		    &datapump->transmitter,
		    datapump->transmit_bit_pending ? datapump->transmit_bit : 1,
		    sample, &consumed) != FSK_OK)
		return FSK_INVALID_ARGUMENT;
	if (consumed)
		datapump->transmit_bit_pending = false;
	return FSK_OK;
}

static enum fsk_result generate_output_sample(
	struct v23_datapump *datapump, int16_t *sample)
{
	switch (datapump->phase) {
	case V23_PHASE_ANSWER_TONE:
		if (generate_fsk_sample(datapump, 0, sample) !=
		    FSK_OK)
			return FSK_INVALID_ARGUMENT;
		datapump->phase_samples++;
		if (datapump->phase_samples == V23_ANSWER_TONE_SAMPLES) {
			datapump->phase = V23_PHASE_ANSWER_SILENCE;
			datapump->phase_samples = 0;
		}
		return FSK_OK;
	case V23_PHASE_ANSWER_SILENCE:
		*sample = 0;
		datapump->phase_samples++;
		if (datapump->phase_samples == V23_ANSWER_SILENCE_SAMPLES) {
			datapump->phase = V23_PHASE_WAIT_MARK;
			datapump->phase_samples = 0;
			fsk_receiver_reset(&datapump->receiver);
		}
		return FSK_OK;
	case V23_PHASE_WAIT_MARK:
	case V23_PHASE_PRESENTATION:
		return generate_fsk_sample(datapump, 1, sample);
	case V23_PHASE_ONLINE:
		return generate_data_sample(datapump, sample);
	}
	return FSK_INVALID_ARGUMENT;
}

static bool receive_input_sample(
	struct v23_datapump *datapump,
	enum v23_phase phase, int16_t sample)
{
	uint8_t bit = 0;
	bool ready = false;

	if (!phase_receives(phase))
		return true;
	if (fsk_receiver_sample(
		    &datapump->receiver, sample, &bit, &ready) !=
	    FSK_OK)
		return false;
	if (!ready || phase == V23_PHASE_WAIT_MARK)
		return true;
	if (phase == V23_PHASE_PRESENTATION) {
		if (datapump->prelink_count == V23_PRELINK_BIT_CAPACITY)
			return false;
		datapump->prelink_bits[datapump->prelink_count++] = bit;
		return true;
	}
	return modem_put_bits(datapump->dp.modem, 1, &bit, 1) == 1;
}

static bool mark_is_present(
	const struct fsk_measurement *measurement)
{
	return measurement->ready &&
	       measurement->line_power >= V23_MIN_LINE_POWER &&
	       measurement->dominance_q8 >= V23_MIN_DOMINANCE_Q8 &&
	       measurement->mark_power > measurement->space_power;
}

static bool carrier_is_present(
	const struct fsk_measurement *measurement)
{
	return measurement->ready &&
	       measurement->line_power >= V23_MIN_LINE_POWER &&
	       measurement->dominance_q8 >= V23_MIN_DOMINANCE_Q8;
}

static void add_bounded(
	uint32_t *value, uint32_t increment, uint32_t limit)
{
	*value = *value >= limit || increment >= limit - *value ?
		       limit : *value + increment;
}

static int enter_online(struct v23_datapump *datapump)
{
	int transmit_rate = datapump->answerer ?
		V23_FORWARD_BIT_RATE :
		V23_BACKWARD_BIT_RATE;
	int receive_rate = datapump->answerer ?
		V23_BACKWARD_BIT_RATE :
		V23_FORWARD_BIT_RATE;

	if (modem_set_param(
		    datapump->dp.modem, MDMPRM_TX_RATE, transmit_rate) != 0 ||
	    modem_set_param(
		    datapump->dp.modem, MDMPRM_RX_RATE, receive_rate) != 0)
		return DPSTAT_ERROR;
	datapump->phase = V23_PHASE_ONLINE;
	datapump->missing_carrier_samples = 0;
	datapump->dp.status = DPSTAT_CONNECT;
	return DPSTAT_CONNECT;
}

static int advance_training(
	struct v23_datapump *datapump,
	const struct fsk_measurement *measurement,
	uint32_t received_samples)
{
	uint32_t acquisition_limit = datapump->answerer ?
		V23_BACKWARD_MARK_ACQUIRE_SAMPLES :
		V23_FORWARD_MARK_ACQUIRE_SAMPLES;

	if (datapump->phase == V23_PHASE_WAIT_MARK) {
		if (mark_is_present(measurement))
			add_bounded(
				&datapump->acquisition_samples, received_samples,
				acquisition_limit);
		else
			datapump->acquisition_samples = 0;
		if (datapump->acquisition_samples < acquisition_limit)
			return DPSTAT_OK;
		datapump->acquisition_samples = 0;
		if (datapump->answerer)
			return enter_online(datapump);
		datapump->phase = V23_PHASE_PRESENTATION;
		datapump->presentation_samples = 0;
		datapump->prelink_count = 0;
		return DPSTAT_OK;
	}
	if (datapump->phase == V23_PHASE_PRESENTATION) {
		if (!measurement->ready ||
		    measurement->line_power < V23_MIN_LINE_POWER) {
			datapump->phase = V23_PHASE_WAIT_MARK;
			datapump->presentation_samples = 0;
			datapump->prelink_count = 0;
			fsk_receiver_reset(&datapump->receiver);
			return DPSTAT_OK;
		}
		add_bounded(
			&datapump->presentation_samples, received_samples,
			datapump->presentation_limit_samples);
		return datapump->presentation_samples >=
			       datapump->presentation_limit_samples ?
			       enter_online(datapump) : DPSTAT_OK;
	}
	return DPSTAT_OK;
}

static int process_v23(struct dp *dp, void *receive_buffer,
		       void *transmit_buffer, int count)
{
	struct v23_datapump *datapump = v23_from_dp(dp);
	const int16_t *receive = receive_buffer;
	int16_t *transmit = transmit_buffer;
	struct fsk_measurement measurement;
	uint32_t received_samples = 0;

	if (datapump == NULL || receive == NULL || transmit == NULL || count <= 0 ||
	    count > MODEM_FRAG) {
		if (transmit != NULL && count > 0 && count <= MODEM_FRAG)
			memset(transmit, 0, (size_t)count * sizeof(*transmit));
		return DPSTAT_ERROR;
	}
	if (datapump->phase == V23_PHASE_ONLINE &&
	    datapump->prelink_count != 0) {
		if (modem_put_bits(
			    datapump->dp.modem, 1, datapump->prelink_bits,
			    (int)datapump->prelink_count) !=
		    (int)datapump->prelink_count) {
			memset(transmit, 0, (size_t)count * sizeof(*transmit));
			return DPSTAT_ERROR;
		}
		datapump->prelink_count = 0;
	}
	for (int index = 0; index < count; ++index) {
		enum v23_phase receive_phase = datapump->phase;

		if (generate_output_sample(datapump, &transmit[index]) !=
			    FSK_OK ||
		    !receive_input_sample(
			    datapump, receive_phase, receive[index])) {
			memset(
				transmit + index, 0,
				(size_t)(count - index) * sizeof(*transmit));
			return DPSTAT_ERROR;
		}
		if (phase_receives(receive_phase))
			received_samples++;
	}
	if (datapump->phase != V23_PHASE_ONLINE) {
		if (datapump->training_samples >
			    datapump->training_timeout_samples ||
		    (uint32_t)count >
		    datapump->training_timeout_samples -
			    datapump->training_samples)
			return DPSTAT_ERROR;
		datapump->training_samples += (uint32_t)count;
	}
	measurement = fsk_receiver_measure(&datapump->receiver);
	if (datapump->phase != V23_PHASE_ONLINE)
		return advance_training(datapump, &measurement, received_samples);
	if (carrier_is_present(&measurement)) {
		datapump->missing_carrier_samples = 0;
	} else {
		add_bounded(
			&datapump->missing_carrier_samples, (uint32_t)count,
			V23_CARRIER_LOSS_SAMPLES);
	}
	return datapump->missing_carrier_samples >= V23_CARRIER_LOSS_SAMPLES ?
		       DPSTAT_ERROR : DPSTAT_OK;
}

int v23_datapump_register(void)
{
	return modem_dp_register(DP_V23, &v23_operations);
}

int v23_datapump_unregister(void)
{
	return modem_dp_deregister(DP_V23, &v23_operations);
}

bool v23_datapump_is_busy(void)
{
	return v23_operations.use_count != 0;
}
