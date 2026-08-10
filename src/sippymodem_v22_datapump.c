// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_v22_datapump.h"

#include "modem.h"
#include "modem_param.h"
#include "sippymodem_qam.h"
#include "sippymodem_v22_framing.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
	V22_SYMBOL_RATE = 600,
	V22_LOW_CHANNEL_HZ = 1200,
	V22_HIGH_CHANNEL_HZ = 2400,
	V22_PULSE_SPAN_SYMBOLS = 6,
	V22_TRANSMIT_AMPLITUDE = 12000,
	/* Timing acquisition already observes about 100 ms of the answer mark. */
	V22_ANSWER_MARK_CONFIRM_SYMBOLS = 32,
	V22_ORIGINATE_SILENCE_SAMPLES = MODEM_RATE * 456 / 1000,
	/* The answerer includes acquisition in its 270 ms confirmation period. */
	V22_ANSWER_SCRAMBLED_CONFIRM_BITS = 192,
	V22_ORIGINATE_SCRAMBLED_CONFIRM_BITS = 324,
	V22_CONNECT_HOLD_SAMPLES = MODEM_RATE * 765 / 1000,
	V22_CARRIER_LOSS_SAMPLES = MODEM_RATE / 5,
	V22_MIN_LINE_POWER = 1000000,
};

static const struct sippymodem_qam_channel v22_low_channel = {
	.sample_rate = MODEM_RATE,
	.symbol_rate_numerator = V22_SYMBOL_RATE,
	.symbol_rate_denominator = 1,
	.carrier_frequency_hz = V22_LOW_CHANNEL_HZ,
	.rolloff = 0.75,
	.pulse_span_symbols = V22_PULSE_SPAN_SYMBOLS,
};

static const struct sippymodem_qam_channel v22_high_channel = {
	.sample_rate = MODEM_RATE,
	.symbol_rate_numerator = V22_SYMBOL_RATE,
	.symbol_rate_denominator = 1,
	.carrier_frequency_hz = V22_HIGH_CHANNEL_HZ,
	.rolloff = 0.75,
	.pulse_span_symbols = V22_PULSE_SPAN_SYMBOLS,
};

enum sippymodem_v22_training_state {
	V22_TRAINING_ORIGINATE_WAIT_ANSWER_MARK,
	V22_TRAINING_ORIGINATE_SILENCE,
	V22_TRAINING_WAIT_SCRAMBLED_MARK,
	V22_TRAINING_CONNECT_HOLD,
	V22_TRAINING_ONLINE,
};

enum sippymodem_v22_transmit_mode {
	V22_TRANSMIT_SILENCE,
	V22_TRANSMIT_UNSCRAMBLED_MARK,
	V22_TRANSMIT_SCRAMBLED_MARK,
	V22_TRANSMIT_DATA,
};

struct sippymodem_v22_datapump {
	struct dp dp;
	struct sippymodem_qam_modulator modulator;
	struct sippymodem_qam_demodulator demodulator;
	struct sippymodem_v22_symbol_encoder encoder;
	struct sippymodem_v22_scrambler descrambler;
	enum sippymodem_v22_training_state training_state;
	enum sippymodem_v22_transmit_mode transmit_mode;
	uint64_t training_samples;
	uint64_t training_timeout_samples;
	uint32_t phase_samples;
	uint32_t answer_mark_symbols;
	uint32_t scrambled_mark_bits;
	uint32_t missing_carrier_samples;
	uint8_t transmit_quadrant;
	struct sippymodem_qam_point previous_receive_point;
	bool has_previous_receive_point;
	bool originator;
};

static const double V22_PI = 3.1415926535897932384626433832795;

static struct dp *create_v22(
	struct modem *modem, enum DP_ID id, int caller, int sample_rate,
	int max_fragment, struct dp_operations *operations);
static int delete_v22(struct dp *dp);
static int process_v22(
	struct dp *dp, void *receive, void *transmit, int count);

static struct dp_operations v22_operations = {
	.name = "sippymodem V.22/Bell 212A",
	.create = create_v22,
	.delete = delete_v22,
	.process = process_v22,
};

static bool id_uses_v22_carrier(enum DP_ID id)
{
	return id == DP_V22 || id == DP_B212;
}

static struct sippymodem_v22_datapump *v22_from_dp(struct dp *dp)
{
	struct sippymodem_v22_datapump *datapump;

	if (dp == NULL || !id_uses_v22_carrier(dp->id) ||
	    dp->op != &v22_operations ||
	    dp->modem == NULL || dp->dp_data == NULL)
		return NULL;
	datapump = dp->dp_data;
	if (&datapump->dp != dp ||
	    datapump->training_state > V22_TRAINING_ONLINE ||
	    datapump->transmit_mode > V22_TRANSMIT_DATA ||
	    datapump->transmit_quadrant > 3 ||
	    datapump->training_timeout_samples == 0 ||
	    datapump->training_samples > datapump->training_timeout_samples ||
	    datapump->phase_samples > V22_CONNECT_HOLD_SAMPLES ||
	    datapump->answer_mark_symbols > V22_ANSWER_MARK_CONFIRM_SYMBOLS ||
	    datapump->scrambled_mark_bits >
		    V22_ORIGINATE_SCRAMBLED_CONFIRM_BITS ||
	    datapump->missing_carrier_samples > V22_CARRIER_LOSS_SAMPLES ||
	    (datapump->has_previous_receive_point &&
	     (!isfinite(datapump->previous_receive_point.in_phase) ||
	      !isfinite(datapump->previous_receive_point.quadrature))))
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

static bool rate_is_selected(const struct modem *modem)
{
	return modem != NULL && modem->min_rate <= SIPPYMODEM_V22_BIT_RATE &&
	       modem->max_rate >= SIPPYMODEM_V22_BIT_RATE;
}

static struct dp *create_v22(
	struct modem *modem, enum DP_ID id, int caller, int sample_rate,
	int max_fragment, struct dp_operations *operations)
{
	struct sippymodem_v22_datapump *datapump;
	const struct sippymodem_qam_channel *transmit_channel;
	const struct sippymodem_qam_channel *receive_channel;

	if (modem == NULL || !id_uses_v22_carrier(id) ||
	    (caller != 0 && caller != 1) ||
	    sample_rate != MODEM_RATE || max_fragment != MODEM_FRAG ||
	    operations != &v22_operations || operations->use_count == INT_MAX ||
	    !rate_is_selected(modem))
		return NULL;
	datapump = calloc(1, sizeof(*datapump));
	if (datapump == NULL)
		return NULL;
	datapump->originator = caller != 0;
	transmit_channel = datapump->originator ? &v22_low_channel :
						   &v22_high_channel;
	receive_channel = datapump->originator ? &v22_high_channel :
						  &v22_low_channel;
	if (sippymodem_qam_modulator_init(
		    &datapump->modulator, transmit_channel,
		    V22_TRANSMIT_AMPLITUDE) != SIPPYMODEM_QAM_OK ||
	    sippymodem_qam_demodulator_init(
		    &datapump->demodulator, receive_channel) !=
		    SIPPYMODEM_QAM_OK ||
	    sippymodem_v22_symbol_encoder_init(&datapump->encoder, 0) !=
		    SIPPYMODEM_V22_FRAMING_OK) {
		free(datapump);
		return NULL;
	}
	sippymodem_v22_scrambler_reset(&datapump->descrambler);
	datapump->dp = (struct dp){
		.id = id,
		.modem = modem,
		.status = DPSTAT_OK,
		.op = operations,
		.dp_data = datapump,
	};
	datapump->training_timeout_samples = training_timeout(modem);
	if (datapump->originator) {
		datapump->training_state =
			V22_TRAINING_ORIGINATE_WAIT_ANSWER_MARK;
		datapump->transmit_mode = V22_TRANSMIT_SILENCE;
	} else {
		datapump->training_state = V22_TRAINING_WAIT_SCRAMBLED_MARK;
		datapump->transmit_mode = V22_TRANSMIT_UNSCRAMBLED_MARK;
	}
	operations->use_count++;
	return &datapump->dp;
}

static int delete_v22(struct dp *dp)
{
	struct sippymodem_v22_datapump *datapump = v22_from_dp(dp);

	if (datapump == NULL || v22_operations.use_count <= 0)
		return -1;
	v22_operations.use_count--;
	datapump->dp.dp_data = NULL;
	free(datapump);
	return 0;
}

static int start_scrambled_transmit(
	struct sippymodem_v22_datapump *datapump)
{
	if (sippymodem_v22_symbol_encoder_init(
		    &datapump->encoder, datapump->transmit_quadrant) !=
	    SIPPYMODEM_V22_FRAMING_OK)
		return -1;
	datapump->transmit_mode = V22_TRANSMIT_SCRAMBLED_MARK;
	return 0;
}

static int next_transmit_quadrant(
	struct sippymodem_v22_datapump *datapump)
{
	uint8_t bits[2] = { 1, 1 };
	uint8_t quadrant;

	if (datapump->transmit_mode == V22_TRANSMIT_UNSCRAMBLED_MARK) {
		datapump->transmit_quadrant =
			(uint8_t)((datapump->transmit_quadrant + 3u) & 3u);
		return 0;
	}
	if (datapump->transmit_mode == V22_TRANSMIT_DATA &&
	    modem_get_bits(datapump->dp.modem, 1, bits, 2) != 2)
		bits[0] = bits[1] = 1;
	bits[0] &= 1u;
	bits[1] &= 1u;
	if (sippymodem_v22_symbol_encode(
		    &datapump->encoder, bits[0], bits[1], &quadrant) !=
	    SIPPYMODEM_V22_FRAMING_OK)
		return -1;
	datapump->transmit_quadrant = quadrant;
	return 0;
}

static int generate_sample(
	struct sippymodem_v22_datapump *datapump, int16_t *sample)
{
	static const struct sippymodem_qam_point quadrants[] = {
		{ 1.0, 0.0 },
		{ 0.0, 1.0 },
		{ -1.0, 0.0 },
		{ 0.0, -1.0 },
	};
	bool accepted;
	bool can_accept;

	if (datapump->transmit_mode == V22_TRANSMIT_SILENCE) {
		*sample = 0;
		return 0;
	}
	can_accept = sippymodem_qam_modulator_can_accept_symbol(
		&datapump->modulator);
	if (can_accept && next_transmit_quadrant(datapump) != 0)
		return -1;
	if (sippymodem_qam_modulator_sample(
		    &datapump->modulator,
		    &quadrants[datapump->transmit_quadrant], sample, &accepted) !=
		    SIPPYMODEM_QAM_OK ||
	    accepted != can_accept)
		return -1;
	return 0;
}

static int decode_phase_change(
	struct sippymodem_v22_datapump *datapump, uint8_t quarter_turns,
	uint8_t bits[2])
{
	uint8_t dibit;
	uint8_t first_scrambled;
	uint8_t second_scrambled;

	if (sippymodem_v22_differential_decode(
		    0, quarter_turns, &dibit) !=
		    SIPPYMODEM_V22_FRAMING_OK)
		return -1;
	first_scrambled = (uint8_t)(dibit >> 1);
	second_scrambled = (uint8_t)(dibit & 1u);
	if (sippymodem_v22_descramble_bit(
		    &datapump->descrambler, first_scrambled, &bits[0]) !=
		    SIPPYMODEM_V22_FRAMING_OK ||
	    sippymodem_v22_descramble_bit(
		    &datapump->descrambler, second_scrambled, &bits[1]) !=
		    SIPPYMODEM_V22_FRAMING_OK)
		return -1;
	return 0;
}

static uint32_t scrambled_confirmation_target(
	const struct sippymodem_v22_datapump *datapump)
{
	return datapump->originator ? V22_ORIGINATE_SCRAMBLED_CONFIRM_BITS :
					V22_ANSWER_SCRAMBLED_CONFIRM_BITS;
}

static int begin_connect_hold(struct sippymodem_v22_datapump *datapump)
{
	if (!datapump->originator && start_scrambled_transmit(datapump) != 0)
		return -1;
	datapump->training_state = V22_TRAINING_CONNECT_HOLD;
	datapump->phase_samples = 0;
	datapump->scrambled_mark_bits = 0;
	return 0;
}

static int observe_phase_change(
	struct sippymodem_v22_datapump *datapump, uint8_t quarter_turns)
{
	uint8_t bits[2];

	if (datapump->training_state ==
	    V22_TRAINING_ORIGINATE_WAIT_ANSWER_MARK) {
		if (quarter_turns == 3)
			datapump->answer_mark_symbols++;
		else
			datapump->answer_mark_symbols = 0;
		if (datapump->answer_mark_symbols >=
		    V22_ANSWER_MARK_CONFIRM_SYMBOLS) {
			datapump->training_state =
				V22_TRAINING_ORIGINATE_SILENCE;
			datapump->phase_samples = 0;
		}
	}
	if (decode_phase_change(datapump, quarter_turns, bits) != 0)
		return -1;
	if (datapump->training_state == V22_TRAINING_WAIT_SCRAMBLED_MARK) {
		for (size_t index = 0; index < sizeof(bits); ++index) {
			if (bits[index] == 1)
				datapump->scrambled_mark_bits++;
			else
				datapump->scrambled_mark_bits = 0;
			if (datapump->scrambled_mark_bits >=
			    scrambled_confirmation_target(datapump))
				return begin_connect_hold(datapump);
		}
	} else if (datapump->training_state == V22_TRAINING_ONLINE &&
		   datapump->dp.modem->put_bits != NULL) {
		(void)modem_put_bits(datapump->dp.modem, 1, bits, 2);
	}
	return 0;
}

static int receive_sample(
	struct sippymodem_v22_datapump *datapump, int16_t sample)
{
	struct sippymodem_qam_point point;
	bool ready;

	if (sippymodem_qam_demodulator_sample(
		    &datapump->demodulator, sample, &point, &ready) !=
	    SIPPYMODEM_QAM_OK)
		return -1;
	if (ready) {
		if (datapump->has_previous_receive_point) {
			double dot = datapump->previous_receive_point.in_phase *
					     point.in_phase +
				     datapump->previous_receive_point.quadrature *
					     point.quadrature;
			double cross = datapump->previous_receive_point.in_phase *
					       point.quadrature -
				       datapump->previous_receive_point.quadrature *
					       point.in_phase;
			long turns = lround(atan2(cross, dot) / (V22_PI / 2.0));

			if (observe_phase_change(
				    datapump,
				    (uint8_t)((unsigned long)turns & 3u)) != 0)
				return -1;
		}
		datapump->previous_receive_point = point;
		datapump->has_previous_receive_point = true;
	}
	return 0;
}

static int enter_link(struct sippymodem_v22_datapump *datapump)
{
	if (modem_set_param(
		    datapump->dp.modem, MDMPRM_TX_RATE,
		    SIPPYMODEM_V22_BIT_RATE) != 0 ||
	    modem_set_param(
		    datapump->dp.modem, MDMPRM_RX_RATE,
		    SIPPYMODEM_V22_BIT_RATE) != 0)
		return -1;
	datapump->training_state = V22_TRAINING_ONLINE;
	datapump->transmit_mode = V22_TRANSMIT_DATA;
	datapump->missing_carrier_samples = 0;
	datapump->dp.status = DPSTAT_CONNECT;
	return 0;
}

static int advance_training_timer(
	struct sippymodem_v22_datapump *datapump, bool *linked)
{
	if (datapump->training_state == V22_TRAINING_ORIGINATE_SILENCE) {
		datapump->phase_samples++;
		if (datapump->phase_samples >= V22_ORIGINATE_SILENCE_SAMPLES) {
			if (start_scrambled_transmit(datapump) != 0)
				return -1;
			datapump->training_state =
				V22_TRAINING_WAIT_SCRAMBLED_MARK;
			datapump->phase_samples = 0;
			datapump->scrambled_mark_bits = 0;
		}
	} else if (datapump->training_state == V22_TRAINING_CONNECT_HOLD) {
		datapump->phase_samples++;
		if (datapump->phase_samples >= V22_CONNECT_HOLD_SAMPLES) {
			if (enter_link(datapump) != 0)
				return -1;
			*linked = true;
		}
	}
	return 0;
}

static bool carrier_is_present(
	const struct sippymodem_qam_measurement *measurement,
	const int16_t *samples, int count)
{
	uint64_t power = 0;

	for (int index = 0; index < count; ++index) {
		int32_t sample = samples[index];

		power += (uint64_t)(sample * sample);
	}
	return measurement->timing_locked &&
	       power / (unsigned int)count >= V22_MIN_LINE_POWER;
}

static void add_bounded(
	uint32_t *value, uint32_t increment, uint32_t limit)
{
	*value = *value >= limit || increment >= limit - *value ?
		       limit : *value + increment;
}

static int process_v22(
	struct dp *dp, void *receive_buffer, void *transmit_buffer, int count)
{
	struct sippymodem_v22_datapump *datapump = v22_from_dp(dp);
	const int16_t *receive = receive_buffer;
	int16_t *transmit = transmit_buffer;
	struct sippymodem_qam_measurement measurement;
	bool linked = false;

	if (datapump == NULL || receive == NULL || transmit == NULL ||
	    count <= 0 || count > MODEM_FRAG) {
		if (transmit != NULL && count > 0 && count <= MODEM_FRAG)
			memset(transmit, 0, (size_t)count * sizeof(*transmit));
		return DPSTAT_ERROR;
	}
	if (datapump->training_state != V22_TRAINING_ONLINE) {
		if ((uint64_t)(unsigned int)count >
		    datapump->training_timeout_samples - datapump->training_samples)
			return DPSTAT_ERROR;
		datapump->training_samples += (unsigned int)count;
	}
	for (int index = 0; index < count; ++index) {
		if (generate_sample(datapump, &transmit[index]) != 0 ||
		    receive_sample(datapump, receive[index]) != 0 ||
		    advance_training_timer(datapump, &linked) != 0) {
			memset(transmit, 0, (size_t)count * sizeof(*transmit));
			return DPSTAT_ERROR;
		}
	}
	if (linked)
		return DPSTAT_CONNECT;
	if (datapump->training_state != V22_TRAINING_ONLINE)
		return DPSTAT_OK;
	measurement = sippymodem_qam_demodulator_measure(
		&datapump->demodulator);
	if (carrier_is_present(&measurement, receive, count)) {
		datapump->missing_carrier_samples = 0;
	} else {
		add_bounded(
			&datapump->missing_carrier_samples,
			(uint32_t)(unsigned int)count,
			V22_CARRIER_LOSS_SAMPLES);
	}
	return datapump->missing_carrier_samples >= V22_CARRIER_LOSS_SAMPLES ?
		       DPSTAT_ERROR : DPSTAT_OK;
}

int sippymodem_v22_datapump_register(void)
{
	if (modem_dp_register(DP_V22, &v22_operations) != 0)
		return -1;
	if (modem_dp_register(DP_B212, &v22_operations) != 0) {
		(void)modem_dp_deregister(DP_V22, &v22_operations);
		return -1;
	}
	return 0;
}

int sippymodem_v22_datapump_unregister(void)
{
	if (modem_dp_deregister(DP_B212, &v22_operations) != 0)
		return -1;
	if (modem_dp_deregister(DP_V22, &v22_operations) != 0) {
		(void)modem_dp_register(DP_B212, &v22_operations);
		return -1;
	}
	return 0;
}

bool sippymodem_v22_datapump_is_busy(void)
{
	return v22_operations.use_count != 0;
}
