// SPDX-License-Identifier: BSD-3-Clause

#include "v22_datapump.h"

#include "datapump_timing.h"
#include "modem.h"
#include "modem_param.h"
#include "qam.h"
#include "v22_framing.h"
#include "v22bis_receiver.h"
#include "v22bis_symbols.h"

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
	V22BIS_ORIGINATE_ACQUISITION_SYMBOLS = 64,
	V22BIS_ANSWER_ACQUISITION_SYMBOLS = 32,
	V22BIS_RECEIVE_START_SAMPLES = MODEM_RATE * 450 / 1000,
	V22BIS_TRANSMIT_START_SAMPLES = MODEM_RATE * 600 / 1000,
	V22BIS_CONFIRM_BITS = 32,
	V22BIS_CONFIRM_SAMPLES = MODEM_RATE / 5,
	/* Require a stable 80 ms suffix, but do not respond until its end. */
	V22_RETRAIN_MIN_SYMBOLS = 48,
	V22BIS_RETRAIN_MIN_VALID_SYMBOLS = 40,
	V22_RETRAIN_MAX_SYMBOLS = 64,
	V22_RETRAIN_CONFIRM_BITS = 32,
	V22_RETRAIN_CONFIRM_SAMPLES = MODEM_RATE / 5,
	V22_RETRAIN_RETRY_SAMPLES = MODEM_RATE * 12 / 10,
};

static const double V22BIS_RETRAIN_POINT_ERROR_RATIO = 0.18;
static const double V22BIS_RETRAIN_MIN_POWER_RATIO = 0.75;
static const double V22BIS_RETRAIN_MAX_POWER_RATIO = 1.25;

static const struct qam_channel v22_low_channel = {
	.sample_rate = MODEM_RATE,
	.symbol_rate_numerator = V22_SYMBOL_RATE,
	.symbol_rate_denominator = 1,
	.carrier_frequency_numerator = V22_LOW_CHANNEL_HZ,
	.carrier_frequency_denominator = 1,
	.rolloff = 0.75,
	.pulse_span_symbols = V22_PULSE_SPAN_SYMBOLS,
};

static const struct qam_channel v22_high_channel = {
	.sample_rate = MODEM_RATE,
	.symbol_rate_numerator = V22_SYMBOL_RATE,
	.symbol_rate_denominator = 1,
	.carrier_frequency_numerator = V22_HIGH_CHANNEL_HZ,
	.carrier_frequency_denominator = 1,
	.rolloff = 0.75,
	.pulse_span_symbols = V22_PULSE_SPAN_SYMBOLS,
};

enum v22_training_state {
	V22_TRAINING_ORIGINATE_WAIT_ANSWER_MARK,
	V22_TRAINING_ORIGINATE_SILENCE,
	V22_TRAINING_V22BIS_RATE_SIGNAL,
	V22_TRAINING_WAIT_SCRAMBLED_MARK,
	V22_TRAINING_CONNECT_HOLD,
	V22_TRAINING_V22BIS_CONFIRM,
	V22_TRAINING_RETRAIN_SIGNAL,
	V22_TRAINING_RETRAIN_WAIT_PEER,
	V22_TRAINING_RETRAIN_MARK,
	V22_TRAINING_ONLINE,
};

enum v22_transmit_mode {
	V22_TRANSMIT_SILENCE,
	V22_TRANSMIT_UNSCRAMBLED_MARK,
	V22_TRANSMIT_V22BIS_RATE_SIGNAL,
	V22_TRANSMIT_SCRAMBLED_MARK,
	V22_TRANSMIT_V22BIS_MARK,
	V22_TRANSMIT_RETRAIN_SIGNAL,
	V22_TRANSMIT_DATA,
};

struct v22_datapump {
	struct dp dp;
	struct qam_modulator modulator;
	struct qam_demodulator demodulator;
	struct v22_symbol_encoder encoder;
	struct v22_symbol_decoder decoder;
	struct v22bis_symbol_encoder v22bis_encoder;
	struct v22bis_receiver v22bis_receiver;
	struct v22bis_rate_signal_detector rate_signal_detector;
	enum v22_training_state training_state;
	enum v22_transmit_mode transmit_mode;
	uint64_t training_samples;
	uint64_t training_timeout_samples;
	uint32_t phase_samples;
	uint32_t answer_mark_symbols;
	uint32_t scrambled_mark_bits;
	uint32_t v22bis_confirm_bits;
	uint32_t rate_signal_tx_symbols;
	uint32_t missing_carrier_samples;
	uint32_t retrain_peer_samples;
	uint32_t retrain_retry_samples;
	uint8_t retrain_detect_symbols;
	uint8_t retrain_valid_symbols;
	uint8_t retrain_previous_turn;
	uint16_t retrain_candidate_bit_count;
	uint8_t retrain_candidate_bits[
		V22_RETRAIN_MAX_SYMBOLS * V22BIS_BITS_PER_SYMBOL];
	unsigned int bit_rate;
	uint8_t transmit_quadrant;
	uint8_t transmit_symbol;
	struct qam_point previous_receive_point;
	struct qam_gain retrain_gain;
	bool has_previous_receive_point;
	bool v22bis_receiver_initialized;
	bool receiving_v22bis;
	bool retraining;
	bool retrain_gain_valid;
	bool retrain_peer_signal_received;
	bool retrain_receive_ready;
	bool retrain_candidate_deliver;
	bool originator;
};

static struct dp *create_v22(
	struct modem *modem, enum DP_ID id, int caller, int sample_rate,
	int max_fragment, struct dp_operations *operations);
static int delete_v22(struct dp *dp);
static int process_v22(
	struct dp *dp, void *receive, void *transmit, int count);
static int retrain_v22(struct dp *dp);
static int enter_link(struct v22_datapump *datapump);
static void flush_retrain_candidate(struct v22_datapump *datapump);
static void add_bounded(
	uint32_t *value, uint32_t increment, uint32_t limit);

static struct dp_operations v22_operations = {
	.name = "V.22/V.22bis/Bell 212A",
	.create = create_v22,
	.delete = delete_v22,
	.process = process_v22,
	.retrain = retrain_v22,
};

static const enum DP_ID v22_ids[] = { DP_V22, DP_V22BIS, DP_B212 };

static bool id_uses_v22_carrier(enum DP_ID id)
{
	return id == DP_V22 || id == DP_V22BIS || id == DP_B212;
}

static bool bit_rate_matches_id(enum DP_ID id, unsigned int bit_rate)
{
	if (id == DP_V22)
		return bit_rate == V22_600_BIT_RATE ||
		       bit_rate == V22_1200_BIT_RATE;
	if (id == DP_V22BIS)
		return bit_rate == V22BIS_BIT_RATE;
	return id == DP_B212 && bit_rate == V22_1200_BIT_RATE;
}

static bool datapump_uses_600(
	const struct v22_datapump *datapump)
{
	return datapump != NULL &&
	       datapump->bit_rate == V22_600_BIT_RATE;
}

static bool datapump_uses_v22bis(
	const struct v22_datapump *datapump)
{
	return datapump != NULL && datapump->dp.id == DP_V22BIS;
}

static bool transmit_uses_v22bis_symbols(
	const struct v22_datapump *datapump)
{
	return datapump != NULL &&
	       (datapump->transmit_mode == V22_TRANSMIT_V22BIS_MARK ||
		(datapump->transmit_mode == V22_TRANSMIT_DATA &&
		 datapump_uses_v22bis(datapump)));
}

static struct v22_datapump *v22_from_dp(struct dp *dp)
{
	struct v22_datapump *datapump;

	if (dp == NULL || !id_uses_v22_carrier(dp->id) ||
	    dp->op != &v22_operations ||
	    dp->modem == NULL || dp->dp_data == NULL)
		return NULL;
	datapump = dp->dp_data;
	if (&datapump->dp != dp ||
	    !bit_rate_matches_id(datapump->dp.id, datapump->bit_rate) ||
	    datapump->training_state > V22_TRAINING_ONLINE ||
	    datapump->transmit_mode > V22_TRANSMIT_DATA ||
	    datapump->transmit_quadrant > 3 ||
	    datapump->training_timeout_samples == 0 ||
	    datapump->training_samples > datapump->training_timeout_samples ||
	    datapump->phase_samples > V22_CONNECT_HOLD_SAMPLES ||
	    datapump->answer_mark_symbols > V22_ANSWER_MARK_CONFIRM_SYMBOLS ||
	    datapump->scrambled_mark_bits >
		    V22_ORIGINATE_SCRAMBLED_CONFIRM_BITS ||
	    datapump->v22bis_confirm_bits > V22BIS_CONFIRM_BITS ||
	    datapump->rate_signal_tx_symbols >
		    V22BIS_RATE_SIGNAL_SYMBOLS ||
	    datapump->retrain_detect_symbols >
		    V22_RETRAIN_MAX_SYMBOLS ||
	    datapump->retrain_valid_symbols >
		    datapump->retrain_detect_symbols ||
	    datapump->retrain_previous_turn >= QPSK_QUADRANT_COUNT ||
	    datapump->retrain_candidate_bit_count >
		    sizeof(datapump->retrain_candidate_bits) ||
	    datapump->retrain_peer_samples > V22_RETRAIN_RETRY_SAMPLES ||
	    datapump->retrain_retry_samples > V22_RETRAIN_RETRY_SAMPLES ||
	    datapump->missing_carrier_samples > V22_CARRIER_LOSS_SAMPLES ||
	    datapump->transmit_symbol >= V22BIS_SYMBOL_COUNT ||
	    datapump->encoder.phase_quadrant >= QPSK_QUADRANT_COUNT ||
	    !v22_scrambler_is_valid(&datapump->encoder.scrambler) ||
	    datapump->decoder.phase_quadrant >= QPSK_QUADRANT_COUNT ||
	    !v22_scrambler_is_valid(&datapump->decoder.descrambler) ||
	    !v22bis_rate_signal_detector_is_valid(
		    &datapump->rate_signal_detector) ||
	    (datapump->has_previous_receive_point &&
	     (!isfinite(datapump->previous_receive_point.in_phase) ||
	      !isfinite(datapump->previous_receive_point.quadrature))) ||
	    (datapump->retrain_gain_valid &&
	     !qam_gain_is_valid(&datapump->retrain_gain)) ||
	    (datapump->v22bis_receiver_initialized &&
	     (!datapump->has_previous_receive_point ||
	      !v22bis_receiver_is_valid(
		      &datapump->v22bis_receiver))) ||
	    (transmit_uses_v22bis_symbols(datapump) &&
	     (datapump->v22bis_encoder.phase_quadrant >=
		      QPSK_QUADRANT_COUNT ||
	      !v22_scrambler_is_valid(
		      &datapump->v22bis_encoder.scrambler))) ||
	    (datapump->receiving_v22bis &&
	     (!datapump->v22bis_receiver_initialized ||
	      !datapump->v22bis_receiver.v22bis_started)) ||
	    (datapump->training_state != V22_TRAINING_WAIT_SCRAMBLED_MARK &&
	     (datapump->rate_signal_detector.present ||
	      datapump->rate_signal_detector.alternating_symbols != 0)) ||
	    (!datapump_uses_v22bis(datapump) &&
	     (datapump->v22bis_receiver_initialized ||
	      datapump->receiving_v22bis || datapump->v22bis_confirm_bits != 0 ||
	      datapump->transmit_mode == V22_TRANSMIT_V22BIS_RATE_SIGNAL ||
	      datapump->transmit_mode == V22_TRANSMIT_V22BIS_MARK)) ||
	    (datapump->retraining &&
	     datapump->training_state != V22_TRAINING_RETRAIN_SIGNAL &&
	     datapump->training_state != V22_TRAINING_RETRAIN_WAIT_PEER &&
	     datapump->training_state != V22_TRAINING_RETRAIN_MARK &&
	     datapump->training_state != V22_TRAINING_V22BIS_CONFIRM) ||
	    (!datapump->retraining &&
	     (datapump->training_state == V22_TRAINING_RETRAIN_SIGNAL ||
	      datapump->training_state == V22_TRAINING_RETRAIN_WAIT_PEER ||
	      datapump->training_state == V22_TRAINING_RETRAIN_MARK ||
	      datapump->retrain_peer_signal_received ||
	      datapump->retrain_receive_ready ||
	      datapump->retrain_peer_samples != 0 ||
	      datapump->retrain_retry_samples != 0)) ||
	    (datapump->retrain_candidate_bit_count == 0 &&
	     datapump->retrain_candidate_deliver) ||
	    (datapump->training_state == V22_TRAINING_RETRAIN_WAIT_PEER &&
	     datapump->retrain_peer_signal_received) ||
	    ((datapump->training_state == V22_TRAINING_RETRAIN_MARK ||
	      (datapump->retraining &&
	       datapump->training_state == V22_TRAINING_V22BIS_CONFIRM)) &&
	     !datapump->retrain_peer_signal_received) ||
	    (datapump_uses_v22bis(datapump) &&
	     (datapump->retraining || datapump->retrain_detect_symbols != 0) &&
	     !datapump->retrain_gain_valid) ||
	    (!datapump_uses_v22bis(datapump) &&
	     datapump->retrain_gain_valid) ||
	    ((datapump->transmit_mode == V22_TRANSMIT_RETRAIN_SIGNAL) !=
	     (datapump->training_state == V22_TRAINING_RETRAIN_SIGNAL)))
		return NULL;
	return datapump;
}

static unsigned int selected_bit_rate(
	const struct modem *modem, enum DP_ID id)
{
	if (modem == NULL)
		return 0;
	if (id == DP_V22BIS)
		return modem->min_rate <= V22BIS_BIT_RATE &&
		       modem->max_rate >= V22BIS_BIT_RATE ?
			       V22BIS_BIT_RATE : 0;
	if (id == DP_B212)
		return modem->min_rate <= V22_1200_BIT_RATE &&
		       modem->max_rate >= V22_1200_BIT_RATE ?
			       V22_1200_BIT_RATE : 0;
	if (id != DP_V22)
		return 0;
	if (modem->min_rate <= V22_1200_BIT_RATE &&
	    modem->max_rate >= V22_1200_BIT_RATE)
		return V22_1200_BIT_RATE;
	return modem->min_rate <= V22_600_BIT_RATE &&
	       modem->max_rate >= V22_600_BIT_RATE ?
		       V22_600_BIT_RATE : 0;
}

static struct dp *create_v22(
	struct modem *modem, enum DP_ID id, int caller, int sample_rate,
	int max_fragment, struct dp_operations *operations)
{
	struct v22_datapump *datapump;
	const struct qam_channel *transmit_channel;
	const struct qam_channel *receive_channel;
	unsigned int bit_rate = selected_bit_rate(modem, id);

	if (modem == NULL || !id_uses_v22_carrier(id) ||
	    (caller != 0 && caller != 1) ||
	    sample_rate != MODEM_RATE || max_fragment != MODEM_FRAG ||
	    operations != &v22_operations || operations->use_count == INT_MAX ||
	    bit_rate == 0)
		return NULL;
	datapump = calloc(1, sizeof(*datapump));
	if (datapump == NULL)
		return NULL;
	datapump->originator = caller != 0;
	transmit_channel = datapump->originator ? &v22_low_channel :
						   &v22_high_channel;
	receive_channel = datapump->originator ? &v22_high_channel :
						  &v22_low_channel;
	if (qam_modulator_init(
		    &datapump->modulator, transmit_channel,
		    V22_TRANSMIT_AMPLITUDE) != QAM_OK ||
	    ((id == DP_V22BIS &&
	      qam_demodulator_init_with_timing_windows(
		      &datapump->demodulator, receive_channel,
		      datapump->originator ?
			      V22BIS_ORIGINATE_ACQUISITION_SYMBOLS :
			      V22BIS_ANSWER_ACQUISITION_SYMBOLS,
		      QAM_DEFAULT_TIMING_WINDOW_SYMBOLS) != QAM_OK) ||
	     (id != DP_V22BIS &&
	      qam_demodulator_init(
		      &datapump->demodulator, receive_channel) != QAM_OK)) ||
	    v22_symbol_encoder_init(&datapump->encoder, 0) !=
		    V22_FRAMING_OK ||
	    v22_symbol_decoder_init(&datapump->decoder, 0) !=
		    V22_FRAMING_OK) {
		free(datapump);
		return NULL;
	}
	datapump->bit_rate = bit_rate;
	datapump->dp = (struct dp){
		.id = id,
		.modem = modem,
		.status = DPSTAT_OK,
		.op = operations,
		.dp_data = datapump,
	};
	datapump->training_timeout_samples =
		datapump_carrier_wait_ticks(modem);
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
	struct v22_datapump *datapump = v22_from_dp(dp);

	if (datapump == NULL || v22_operations.use_count <= 0)
		return -1;
	v22_operations.use_count--;
	datapump->dp.dp_data = NULL;
	free(datapump);
	return 0;
}

static int start_scrambled_transmit(
	struct v22_datapump *datapump)
{
	if (v22_symbol_encoder_init(
		    &datapump->encoder, datapump->transmit_quadrant) !=
	    V22_FRAMING_OK)
		return -1;
	datapump->transmit_mode = V22_TRANSMIT_SCRAMBLED_MARK;
	return 0;
}

static int start_v22bis_rate_signal(
	struct v22_datapump *datapump)
{
	if (!datapump_uses_v22bis(datapump))
		return -1;
	datapump->rate_signal_tx_symbols = 0;
	datapump->transmit_mode = V22_TRANSMIT_V22BIS_RATE_SIGNAL;
	return 0;
}

static int next_transmit_quadrant(
	struct v22_datapump *datapump)
{
	uint8_t bits[2] = { 1, 1 };
	size_t bit_count = datapump_uses_600(datapump) ? 1u : 2u;
	uint8_t quadrant;

	if (datapump->transmit_mode ==
		    V22_TRANSMIT_V22BIS_RATE_SIGNAL ||
	    datapump->transmit_mode == V22_TRANSMIT_RETRAIN_SIGNAL) {
		uint8_t quarter_turns;

		if (v22bis_rate_signal_turn(
			    datapump->rate_signal_tx_symbols,
			    &quarter_turns) != V22_FRAMING_OK)
			return -1;
		datapump->transmit_quadrant = (uint8_t)(
			(datapump->transmit_quadrant + quarter_turns) & 3u);
		datapump->rate_signal_tx_symbols++;
		return 0;
	}
	if (datapump->transmit_mode == V22_TRANSMIT_UNSCRAMBLED_MARK) {
		datapump->transmit_quadrant =
			(uint8_t)((datapump->transmit_quadrant + 3u) & 3u);
		return 0;
	}
	if (datapump->transmit_mode == V22_TRANSMIT_DATA &&
	    modem_get_bits(
		    datapump->dp.modem, 1, bits, (int)bit_count) !=
		    (int)bit_count)
		bits[0] = bits[1] = 1;
	bits[0] &= 1u;
	bits[1] &= 1u;
	if ((datapump_uses_600(datapump) &&
	     v22_600_symbol_encode(
		     &datapump->encoder, bits[0], &quadrant) !=
		     V22_FRAMING_OK) ||
	    (!datapump_uses_600(datapump) &&
	     v22_symbol_encode(
		     &datapump->encoder, bits[0], bits[1], &quadrant) !=
		     V22_FRAMING_OK))
		return -1;
	datapump->transmit_quadrant = quadrant;
	return 0;
}

static int next_transmit_v22bis_symbol(
	struct v22_datapump *datapump)
{
	uint8_t bits[V22BIS_BITS_PER_SYMBOL] = { 1, 1, 1, 1 };

	if (datapump->transmit_mode == V22_TRANSMIT_DATA &&
	    modem_get_bits(
		    datapump->dp.modem, 1, bits,
		    V22BIS_BITS_PER_SYMBOL) !=
		    V22BIS_BITS_PER_SYMBOL)
		for (size_t index = 0;
		     index < V22BIS_BITS_PER_SYMBOL; ++index)
			bits[index] = 1;
	for (size_t index = 0; index < V22BIS_BITS_PER_SYMBOL;
	     ++index)
		bits[index] &= 1u;
	return v22bis_symbol_encode(
		       &datapump->v22bis_encoder, bits,
		       &datapump->transmit_symbol) ==
		       V22_FRAMING_OK ?
		       0 :
		       -1;
}

static int generate_sample(
	struct v22_datapump *datapump, int16_t *sample)
{
	static const struct qam_point quadrants[] = {
		{ 1.0, 0.0 },
		{ 0.0, 1.0 },
		{ -1.0, 0.0 },
		{ 0.0, -1.0 },
	};
	struct qam_point point;
	bool accepted;
	bool can_accept;

	if (datapump->transmit_mode == V22_TRANSMIT_SILENCE) {
		*sample = 0;
		return 0;
	}
	can_accept = qam_modulator_can_accept_symbol(
		&datapump->modulator);
	if (transmit_uses_v22bis_symbols(datapump)) {
		if ((can_accept && next_transmit_v22bis_symbol(datapump) != 0) ||
		    v22bis_constellation_point(
			    datapump->transmit_symbol, &point) !=
			    V22_FRAMING_OK)
			return -1;
	} else {
		if (can_accept && next_transmit_quadrant(datapump) != 0)
			return -1;
		if (datapump_uses_v22bis(datapump)) {
			if (v22bis_1200_constellation_point(
				    datapump->transmit_quadrant, &point) !=
			    V22_FRAMING_OK)
				return -1;
		} else {
			point = quadrants[datapump->transmit_quadrant];
		}
	}
	if (qam_modulator_sample(
		    &datapump->modulator, &point, sample, &accepted) !=
		    QAM_OK ||
	    accepted != can_accept)
		return -1;
	return 0;
}

static int decode_phase_change(
	struct v22_datapump *datapump, uint8_t quarter_turns,
	uint8_t bits[2])
{
	uint8_t quadrant = (uint8_t)(
		(datapump->decoder.phase_quadrant + quarter_turns) & 3u);

	return v22_symbol_decode(
		       &datapump->decoder, quadrant, &bits[0], &bits[1]) ==
		       V22_FRAMING_OK ?
		       0 : -1;
}

static int decode_600_phase_change(
	struct v22_datapump *datapump,
	const struct qam_point *previous,
	const struct qam_point *current, uint8_t *bit)
{
	double cross;
	uint8_t quadrant;
	uint8_t quarter_turns;

	if (!point_is_finite(previous) || !point_is_finite(current) ||
	    bit == NULL)
		return -1;
	cross = previous->in_phase * current->quadrature -
		previous->quadrature * current->in_phase;
	if (!isfinite(cross))
		return -1;
	/* Choose between the two Table 1 rotations before quarter-turn
	 * quantization discards which side of a tie the observation occupied. */
	quarter_turns = cross < 0.0 ? 3u : 1u;
	quadrant = (uint8_t)(
		(datapump->decoder.phase_quadrant + quarter_turns) & 3u);
	return v22_600_symbol_decode(
		       &datapump->decoder, quadrant, bit) == V22_FRAMING_OK ?
		       0 : -1;
}

static uint32_t scrambled_confirmation_target(
	const struct v22_datapump *datapump)
{
	return datapump->originator ? V22_ORIGINATE_SCRAMBLED_CONFIRM_BITS :
					V22_ANSWER_SCRAMBLED_CONFIRM_BITS;
}

static int begin_connect_hold(struct v22_datapump *datapump)
{
	if (!datapump->originator && start_scrambled_transmit(datapump) != 0)
		return -1;
	datapump->training_state = V22_TRAINING_CONNECT_HOLD;
	datapump->phase_samples = 0;
	datapump->scrambled_mark_bits = 0;
	return 0;
}

static int start_v22bis_transmit(
	struct v22_datapump *datapump)
{
	if (!datapump_uses_v22bis(datapump) ||
	    !datapump->v22bis_receiver_initialized ||
	    v22bis_symbol_encoder_init(
		    &datapump->v22bis_encoder,
		    datapump->transmit_quadrant) !=
		    V22_FRAMING_OK)
		return -1;
	datapump->transmit_mode = V22_TRANSMIT_V22BIS_MARK;
	datapump->training_state = V22_TRAINING_V22BIS_CONFIRM;
	datapump->phase_samples = 0;
	return 0;
}

static void observe_answer_mark(
	struct v22_datapump *datapump, uint8_t quarter_turns)
{
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
}

static int output_received_bits(
	struct v22_datapump *datapump, uint8_t *bits,
	size_t bit_count)
{
	if (datapump->retrain_detect_symbols != 0) {
		if (bit_count >
		    sizeof(datapump->retrain_candidate_bits) -
			    datapump->retrain_candidate_bit_count)
			return -1;
		memcpy(datapump->retrain_candidate_bits +
			       datapump->retrain_candidate_bit_count,
		       bits, bit_count);
		datapump->retrain_candidate_bit_count += (uint16_t)bit_count;
	} else if (datapump->dp.modem->put_bits != NULL) {
		(void)modem_put_bits(
			datapump->dp.modem, 1, bits,
			(int)bit_count);
	}
	return 0;
}

static int observe_bits(
	struct v22_datapump *datapump, uint8_t *bits,
	size_t bit_count)
{
	if (datapump->training_state == V22_TRAINING_WAIT_SCRAMBLED_MARK &&
	    !datapump_uses_v22bis(datapump)) {
		for (size_t index = 0; index < bit_count; ++index) {
			if (bits[index] == 1)
				datapump->scrambled_mark_bits++;
			else
				datapump->scrambled_mark_bits = 0;
			if (datapump->scrambled_mark_bits >=
			    scrambled_confirmation_target(datapump))
				return begin_connect_hold(datapump);
		}
	} else if (datapump->retraining &&
		   !datapump_uses_v22bis(datapump) &&
		   datapump->retrain_peer_signal_received) {
		for (size_t index = 0; index < bit_count; ++index) {
			if (!datapump->retrain_receive_ready) {
				if (bits[index] == 0)
					datapump->scrambled_mark_bits = 0;
				else if (datapump->scrambled_mark_bits <
					 V22_RETRAIN_CONFIRM_BITS)
					datapump->scrambled_mark_bits++;
				if (datapump->scrambled_mark_bits >=
				    V22_RETRAIN_CONFIRM_BITS)
					datapump->retrain_receive_ready = true;
			} else if (output_received_bits(
					   datapump, &bits[index], 1) != 0) {
				return -1;
			}
		}
	} else if (datapump->training_state == V22_TRAINING_ONLINE) {
		return output_received_bits(datapump, bits, bit_count);
	}
	return 0;
}

static int observe_phase_change(
	struct v22_datapump *datapump, uint8_t quarter_turns)
{
	uint8_t bits[2];

	observe_answer_mark(datapump, quarter_turns);
	if (decode_phase_change(datapump, quarter_turns, bits) != 0)
		return -1;
	return observe_bits(datapump, bits, sizeof(bits));
}

static int observe_600_phase_change(
	struct v22_datapump *datapump,
	const struct qam_point *previous,
	const struct qam_point *current, uint8_t quarter_turns)
{
	uint8_t bit;

	observe_answer_mark(datapump, quarter_turns);
	if (decode_600_phase_change(datapump, previous, current, &bit) != 0)
		return -1;
	return observe_bits(datapump, &bit, 1);
}

static int begin_v22bis_transition(
	struct v22_datapump *datapump)
{
	if (!datapump_uses_v22bis(datapump) ||
	    datapump->training_state != V22_TRAINING_WAIT_SCRAMBLED_MARK ||
	    datapump->v22bis_receiver_initialized ||
	    datapump->receiving_v22bis)
		return -1;
	if (datapump->originator &&
	    datapump->transmit_mode != V22_TRANSMIT_SCRAMBLED_MARK)
		return -1;
	/* The periodic answer mark can favor a half-symbol timing phase.  Use the
	 * finite rate-signal boundary to reacquire on the scrambled transition. */
	if (qam_demodulator_reset(&datapump->demodulator) != QAM_OK)
		return -1;
	if (!datapump->originator &&
	    start_v22bis_rate_signal(datapump) != 0) {
		return -1;
	}
	datapump->training_state = V22_TRAINING_CONNECT_HOLD;
	datapump->phase_samples = 0;
	datapump->v22bis_confirm_bits = 0;
	v22bis_rate_signal_detector_reset(
		&datapump->rate_signal_detector);
	return 0;
}

static int observe_v22bis_rate_signal(
	struct v22_datapump *datapump, uint8_t quarter_turns,
	bool *handled)
{
	enum v22bis_rate_signal_event event;
	uint8_t phase;

	if (!datapump_uses_v22bis(datapump) || handled == NULL ||
	    datapump->training_state != V22_TRAINING_WAIT_SCRAMBLED_MARK ||
	    v22bis_rate_signal_observe(
		    &datapump->rate_signal_detector, quarter_turns,
		    &event) != V22_FRAMING_OK)
		return -1;
	*handled = false;
	if (event == V22BIS_RATE_SIGNAL_ENDED) {
		phase = datapump->decoder.phase_quadrant;
		if (v22_symbol_decoder_init(
			    &datapump->decoder, phase) != V22_FRAMING_OK ||
		    begin_v22bis_transition(datapump) != 0)
			return -1;
		return 0;
	}
	if (datapump->rate_signal_detector.present) {
		datapump->decoder.phase_quadrant = (uint8_t)(
			(datapump->decoder.phase_quadrant + quarter_turns) & 3u);
		*handled = true;
	}
	return 0;
}

static bool v22bis_is_ready_to_link(
	const struct v22_datapump *datapump)
{
	return datapump->training_state == V22_TRAINING_V22BIS_CONFIRM &&
	       datapump->phase_samples >= V22BIS_CONFIRM_SAMPLES &&
	       (datapump->retraining ? datapump->retrain_receive_ready :
					 datapump->v22bis_confirm_bits >=
						 V22BIS_CONFIRM_BITS);
}

static int observe_v22bis_bits(
	struct v22_datapump *datapump,
	uint8_t bits[V22BIS_BITS_PER_SYMBOL], bool *linked)
{
	if (datapump->training_state == V22_TRAINING_ONLINE)
		return output_received_bits(
			datapump, bits, V22BIS_BITS_PER_SYMBOL);
	if (datapump->retraining && datapump->retrain_receive_ready) {
		if (output_received_bits(
			    datapump, bits, V22BIS_BITS_PER_SYMBOL) != 0)
			return -1;
	} else if (datapump->v22bis_confirm_bits < V22BIS_CONFIRM_BITS) {
		for (size_t index = 0;
		     index < V22BIS_BITS_PER_SYMBOL; ++index) {
			if (bits[index] == 0)
				datapump->v22bis_confirm_bits = 0;
			else if (datapump->v22bis_confirm_bits <
				 V22BIS_CONFIRM_BITS)
				datapump->v22bis_confirm_bits++;
		}
		if (datapump->retraining &&
		    datapump->v22bis_confirm_bits >= V22BIS_CONFIRM_BITS)
			datapump->retrain_receive_ready = true;
	}
	if (v22bis_is_ready_to_link(datapump)) {
		if (enter_link(datapump) != 0)
			return -1;
		*linked = true;
	}
	return 0;
}

static int receive_v22bis_point(
	struct v22_datapump *datapump,
	const struct qam_point *point, bool *linked)
{
	uint8_t bits[V22BIS_BITS_PER_SYMBOL];

	if (!datapump->receiving_v22bis) {
		uint8_t quarter_turns;
		bool matches_v22;

		if (v22bis_receiver_matches_v22(
			    &datapump->v22bis_receiver, point, &matches_v22) !=
		    V22BIS_RECEIVER_OK)
			return -1;
		if (matches_v22) {
			if (qam_nearest_quarter_turn(
				    &datapump->previous_receive_point, point,
				    &quarter_turns) != QAM_OK ||
			    observe_phase_change(datapump, quarter_turns) != 0 ||
			    v22bis_receiver_track_v22(
				    &datapump->v22bis_receiver, quarter_turns,
				    point) != V22BIS_RECEIVER_OK)
				return -1;
			return 0;
		}
	}
	if (v22bis_receiver_decode(
		    &datapump->v22bis_receiver, point, bits) !=
	    V22BIS_RECEIVER_OK)
		return -1;
	datapump->receiving_v22bis = true;
	return observe_v22bis_bits(datapump, bits, linked);
}

static bool v22bis_receive_window_open(
	const struct v22_datapump *datapump)
{
	if (!datapump_uses_v22bis(datapump))
		return false;
	if (datapump->retraining)
		return datapump->retrain_peer_signal_received &&
		       datapump->retrain_peer_samples >=
			       V22BIS_RECEIVE_START_SAMPLES;
	if (datapump->training_state > V22_TRAINING_CONNECT_HOLD)
		return true;
	return datapump->training_state == V22_TRAINING_CONNECT_HOLD &&
	       datapump->phase_samples >= V22BIS_RECEIVE_START_SAMPLES;
}

static uint8_t receive_phase_quadrant(
	const struct v22_datapump *datapump)
{
	if (datapump_uses_v22bis(datapump) &&
	    datapump->v22bis_receiver_initialized)
		return datapump->v22bis_receiver.decoder.phase_quadrant;
	return datapump->decoder.phase_quadrant;
}

static void flush_retrain_candidate(
	struct v22_datapump *datapump)
{
	if (datapump->retrain_candidate_bit_count != 0 &&
	    datapump->retrain_candidate_deliver &&
	    datapump->dp.modem->put_bits != NULL)
		(void)modem_put_bits(
			datapump->dp.modem, 1,
			datapump->retrain_candidate_bits,
			(int)datapump->retrain_candidate_bit_count);
	datapump->retrain_candidate_bit_count = 0;
	datapump->retrain_candidate_deliver = false;
}

static void discard_retrain_candidate(
	struct v22_datapump *datapump)
{
	datapump->retrain_detect_symbols = 0;
	datapump->retrain_valid_symbols = 0;
	datapump->retrain_previous_turn = 0;
	datapump->retrain_candidate_bit_count = 0;
	datapump->retrain_candidate_deliver = false;
}

static size_t retrain_bits_per_symbol(
	const struct v22_datapump *datapump)
{
	if (datapump_uses_v22bis(datapump))
		return V22BIS_BITS_PER_SYMBOL;
	return datapump_uses_600(datapump) ? 1u : 2u;
}

static int shift_retrain_candidate(struct v22_datapump *datapump)
{
	size_t bit_count = retrain_bits_per_symbol(datapump);

	if (datapump->retrain_candidate_bit_count < bit_count)
		return datapump->retrain_candidate_deliver ? -1 : 0;
	if (datapump->retrain_candidate_deliver &&
	    datapump->dp.modem->put_bits != NULL)
		(void)modem_put_bits(
			datapump->dp.modem, 1,
			datapump->retrain_candidate_bits, (int)bit_count);
	datapump->retrain_candidate_bit_count -= (uint16_t)bit_count;
	memmove(datapump->retrain_candidate_bits,
		datapump->retrain_candidate_bits + bit_count,
		datapump->retrain_candidate_bit_count);
	return 0;
}

static bool capture_retrain_gain(struct v22_datapump *datapump)
{
	if (!datapump_uses_v22bis(datapump))
		return true;
	if (!datapump->v22bis_receiver_initialized ||
	    !v22bis_receiver_is_valid(&datapump->v22bis_receiver))
		return false;
	datapump->retrain_gain = datapump->v22bis_receiver.gain;
	datapump->retrain_gain_valid = true;
	return true;
}

static double point_squared_error(
	const struct qam_point *left, const struct qam_point *right)
{
	double in_phase = left->in_phase - right->in_phase;
	double quadrature = left->quadrature - right->quadrature;

	return in_phase * in_phase + quadrature * quadrature;
}

static double point_squared_power(const struct qam_point *point)
{
	return point->in_phase * point->in_phase +
	       point->quadrature * point->quadrature;
}

static int retrain_point_matches(
	const struct v22_datapump *datapump,
	const struct qam_point *point, uint8_t quadrant, bool *matches)
{
	struct qam_point expected;
	struct qam_point predicted;
	double power;
	double observed_power;
	double best_error = HUGE_VAL;

	if (point == NULL || !point_is_finite(point) ||
	    quadrant >= QPSK_QUADRANT_COUNT || matches == NULL)
		return -1;
	if (!datapump_uses_v22bis(datapump)) {
		*matches = true;
		return 0;
	}
	if (!datapump->retrain_gain_valid)
		return -1;
	for (uint8_t candidate = 0; candidate < QPSK_QUADRANT_COUNT;
	     ++candidate) {
		double error;

		if (v22bis_1200_constellation_point(candidate, &expected) !=
			    V22_FRAMING_OK ||
		    qam_gain_predict(
			    &datapump->retrain_gain, &expected,
			    &predicted) != QAM_GAIN_OK)
			return -1;
		error = point_squared_error(point, &predicted);
		if (error < best_error)
			best_error = error;
	}
	power = point_squared_power(&datapump->retrain_gain.channel_gain);
	observed_power = point_squared_power(point);
	if (!isfinite(power) || !isfinite(observed_power) ||
	    !isfinite(best_error) || power <= 0.0)
		return -1;
	*matches = observed_power >=
			   power * V22BIS_RETRAIN_MIN_POWER_RATIO &&
		   observed_power <=
			   power * V22BIS_RETRAIN_MAX_POWER_RATIO &&
		   best_error <=
			   power * V22BIS_RETRAIN_POINT_ERROR_RATIO;
	return 0;
}

static int observe_retrain_signal(
	struct v22_datapump *datapump, const struct qam_point *point,
	uint8_t quarter_turns, bool online, bool *ended,
	uint8_t *current_quadrant)
{
	bool marker_point = true;
	bool alternating;
	bool valid_turn = quarter_turns == 1u || quarter_turns == 3u;
	uint8_t quadrant;

	if (ended == NULL || current_quadrant == NULL)
		return -1;
	*ended = false;
	quadrant = (uint8_t)(
		(receive_phase_quadrant(datapump) + quarter_turns) & 3u);
	*current_quadrant = quadrant;
	alternating = valid_turn &&
		(datapump->retrain_detect_symbols == 0 ||
		 quarter_turns != datapump->retrain_previous_turn);
	if (!alternating) {
		if (datapump->retrain_detect_symbols >=
			    V22_RETRAIN_MIN_SYMBOLS &&
		    (!datapump_uses_v22bis(datapump) ||
		     datapump->retrain_valid_symbols >=
			     V22BIS_RETRAIN_MIN_VALID_SYMBOLS)) {
			discard_retrain_candidate(datapump);
			*ended = true;
			return 0;
		}
		flush_retrain_candidate(datapump);
		datapump->retrain_detect_symbols = 0;
		datapump->retrain_valid_symbols = 0;
		if (valid_turn) {
			/* Freeze the online gain before 16-way decoding adapts to S1. */
			if ((online && !capture_retrain_gain(datapump)) ||
			    retrain_point_matches(
				    datapump, point, quadrant,
				    &marker_point) != 0)
				return -1;
			datapump->retrain_detect_symbols = 1;
			datapump->retrain_valid_symbols = marker_point ? 1 : 0;
			datapump->retrain_candidate_deliver =
				online || datapump->retrain_receive_ready;
		}
	} else {
		if (datapump->retrain_detect_symbols == 0) {
			/* Keep this gain fixed for the whole alternating-phase run. */
			if (online && !capture_retrain_gain(datapump))
				return -1;
			datapump->retrain_candidate_deliver =
				online || datapump->retrain_receive_ready;
		}
		if (retrain_point_matches(
			    datapump, point, quadrant, &marker_point) != 0)
			return -1;
		if (datapump->retrain_detect_symbols <
		    V22_RETRAIN_MAX_SYMBOLS)
			datapump->retrain_detect_symbols++;
		else if (shift_retrain_candidate(datapump) != 0)
			return -1;
		if (!marker_point)
			datapump->retrain_valid_symbols = 0;
		else if (datapump->retrain_valid_symbols <
			 V22_RETRAIN_MAX_SYMBOLS)
			datapump->retrain_valid_symbols++;
	}
	datapump->retrain_previous_turn = quarter_turns;
	return 0;
}

static int start_retrain_signal(struct v22_datapump *datapump)
{
	if (transmit_uses_v22bis_symbols(datapump))
		datapump->transmit_quadrant =
			datapump->v22bis_encoder.phase_quadrant;
	datapump->training_state = V22_TRAINING_RETRAIN_SIGNAL;
	datapump->transmit_mode = V22_TRANSMIT_RETRAIN_SIGNAL;
	datapump->rate_signal_tx_symbols = 0;
	datapump->retrain_retry_samples = 0;
	return 0;
}

static int begin_retrain(
	struct v22_datapump *datapump, uint8_t receive_quadrant,
	bool peer_signal_received)
{
	if (datapump == NULL || datapump->training_state != V22_TRAINING_ONLINE ||
	    receive_quadrant >= QPSK_QUADRANT_COUNT ||
	    (datapump_uses_v22bis(datapump) &&
	     !datapump->retrain_gain_valid && !capture_retrain_gain(datapump)) ||
	    v22_symbol_decoder_init(
		    &datapump->decoder, receive_quadrant) != V22_FRAMING_OK)
		return -1;
	datapump->training_samples = 0;
	datapump->phase_samples = 0;
	datapump->scrambled_mark_bits = 0;
	datapump->v22bis_confirm_bits = 0;
	datapump->missing_carrier_samples = 0;
	datapump->retrain_peer_samples = 0;
	discard_retrain_candidate(datapump);
	datapump->v22bis_receiver_initialized = false;
	datapump->receiving_v22bis = false;
	datapump->retraining = true;
	datapump->retrain_peer_signal_received = peer_signal_received;
	datapump->retrain_receive_ready = false;
	v22bis_rate_signal_detector_reset(&datapump->rate_signal_detector);
	if (!datapump->dp.modem->cfg.ec)
		modem_async_reframe(datapump->dp.modem);
	if (start_retrain_signal(datapump) != 0)
		return -1;
	datapump->dp.status = DPSTAT_OK;
	return 0;
}

static int restart_retrain(struct v22_datapump *datapump)
{
	uint8_t receive_quadrant = receive_phase_quadrant(datapump);

	if (!datapump->retraining ||
	    v22_symbol_decoder_init(
		    &datapump->decoder, receive_quadrant) != V22_FRAMING_OK)
		return -1;
	datapump->phase_samples = 0;
	datapump->scrambled_mark_bits = 0;
	datapump->v22bis_confirm_bits = 0;
	datapump->retrain_peer_samples = 0;
	discard_retrain_candidate(datapump);
	datapump->v22bis_receiver_initialized = false;
	datapump->receiving_v22bis = false;
	datapump->retrain_peer_signal_received = false;
	datapump->retrain_receive_ready = false;
	if (!datapump->dp.modem->cfg.ec)
		modem_async_reframe(datapump->dp.modem);
	return start_retrain_signal(datapump);
}

static int note_peer_retrain_signal(
	struct v22_datapump *datapump, uint8_t receive_quadrant)
{
	if (!datapump->retraining ||
	    datapump->retrain_peer_signal_received ||
	    receive_quadrant >= QPSK_QUADRANT_COUNT ||
	    v22_symbol_decoder_init(
		    &datapump->decoder, receive_quadrant) != V22_FRAMING_OK)
		return -1;
	datapump->retrain_peer_signal_received = true;
	datapump->retrain_peer_samples = 0;
	datapump->retrain_retry_samples = 0;
	datapump->scrambled_mark_bits = 0;
	datapump->v22bis_confirm_bits = 0;
	datapump->retrain_receive_ready = false;
	discard_retrain_candidate(datapump);
	if (datapump->training_state == V22_TRAINING_RETRAIN_WAIT_PEER)
		datapump->training_state = V22_TRAINING_RETRAIN_MARK;
	else if (datapump->training_state != V22_TRAINING_RETRAIN_SIGNAL)
		return -1;
	return 0;
}

static int repeat_peer_retrain_signal(
	struct v22_datapump *datapump, uint8_t receive_quadrant)
{
	if (!datapump->retraining ||
	    !datapump->retrain_peer_signal_received ||
	    receive_quadrant >= QPSK_QUADRANT_COUNT ||
	    v22_symbol_decoder_init(
		    &datapump->decoder, receive_quadrant) != V22_FRAMING_OK)
		return -1;
	datapump->phase_samples = 0;
	datapump->scrambled_mark_bits = 0;
	datapump->v22bis_confirm_bits = 0;
	datapump->retrain_peer_samples = 0;
	discard_retrain_candidate(datapump);
	datapump->v22bis_receiver_initialized = false;
	datapump->receiving_v22bis = false;
	datapump->retrain_receive_ready = false;
	if (!datapump->dp.modem->cfg.ec)
		modem_async_reframe(datapump->dp.modem);
	return start_retrain_signal(datapump);
}

static int retrain_v22(struct dp *dp)
{
	struct v22_datapump *datapump = v22_from_dp(dp);

	if (datapump == NULL || datapump->dp.id == DP_B212)
		return -1;
	flush_retrain_candidate(datapump);
	discard_retrain_candidate(datapump);
	datapump->retrain_gain_valid = false;
	return begin_retrain(
		datapump, receive_phase_quadrant(datapump), false);
}

static int detect_online_retrain(
	struct v22_datapump *datapump, const struct qam_point *point,
	bool *started)
{
	uint8_t current_quadrant;
	uint8_t quarter_turns;
	bool ended;

	if (started == NULL)
		return -1;
	*started = false;
	if (!datapump->has_previous_receive_point)
		return 0;
	if (qam_nearest_quarter_turn(
		    &datapump->previous_receive_point, point,
		    &quarter_turns) != QAM_OK ||
	    observe_retrain_signal(
		    datapump, point, quarter_turns, true, &ended,
		    &current_quadrant) != 0)
		return -1;
	if (!ended)
		return 0;
	if (begin_retrain(datapump, current_quadrant, true) != 0)
		return -1;
	*started = true;
	return 0;
}

static int detect_peer_retrain(
	struct v22_datapump *datapump, const struct qam_point *point,
	bool decode_current, bool *ended)
{
	uint8_t current_quadrant;
	uint8_t quarter_turns;

	if (!datapump->has_previous_receive_point || ended == NULL)
		return -1;
	if (qam_nearest_quarter_turn(
		    &datapump->previous_receive_point, point,
		    &quarter_turns) != QAM_OK ||
	    observe_retrain_signal(
		    datapump, point, quarter_turns, false, ended,
		    &current_quadrant) != 0)
		return -1;
	if (*ended)
		return datapump->retrain_peer_signal_received ?
			repeat_peer_retrain_signal(
				datapump, current_quadrant) :
			note_peer_retrain_signal(
				datapump, current_quadrant);
	if (!decode_current)
		datapump->decoder.phase_quadrant = current_quadrant;
	return 0;
}

static int receive_sample(
	struct v22_datapump *datapump, int16_t sample, bool *linked)
{
	struct qam_point point;
	uint8_t quarter_turns;
	bool ready;

	if (qam_demodulator_sample(
		    &datapump->demodulator, sample, &point, &ready) !=
	    QAM_OK)
		return -1;
	if (!ready)
		return 0;
	if (datapump->retraining) {
		bool decode_current =
			datapump->retrain_peer_signal_received;
		bool ended;

		if (detect_peer_retrain(
			    datapump, &point, decode_current, &ended) != 0)
			return -1;
		if (ended || !decode_current)
			goto remember_point;
		if (datapump_uses_v22bis(datapump) &&
			   datapump->v22bis_receiver_initialized &&
			   v22bis_receive_window_open(datapump)) {
			if (receive_v22bis_point(
				    datapump, &point, linked) != 0)
				return -1;
		} else if (datapump->has_previous_receive_point &&
			   qam_nearest_quarter_turn(
				   &datapump->previous_receive_point, &point,
				   &quarter_turns) == QAM_OK) {
			if ((datapump_uses_600(datapump) &&
			     observe_600_phase_change(
				     datapump,
				     &datapump->previous_receive_point,
				     &point, quarter_turns) != 0) ||
			    (!datapump_uses_600(datapump) &&
			     observe_phase_change(
				     datapump, quarter_turns) != 0))
				return -1;
		} else if (datapump->has_previous_receive_point) {
			return -1;
		}
	} else if (datapump->training_state == V22_TRAINING_ONLINE) {
		bool retrain_started;

		if (detect_online_retrain(
			    datapump, &point, &retrain_started) != 0)
			return -1;
		if (retrain_started)
			goto remember_point;
		if (datapump_uses_v22bis(datapump)) {
			if (receive_v22bis_point(
				    datapump, &point, linked) != 0)
				return -1;
		} else if (datapump->has_previous_receive_point &&
			   qam_nearest_quarter_turn(
				   &datapump->previous_receive_point, &point,
				   &quarter_turns) == QAM_OK) {
			if ((datapump_uses_600(datapump) &&
			     observe_600_phase_change(
				     datapump,
				     &datapump->previous_receive_point,
				     &point, quarter_turns) != 0) ||
			    (!datapump_uses_600(datapump) &&
			     observe_phase_change(
				     datapump, quarter_turns) != 0))
				return -1;
		} else if (datapump->has_previous_receive_point) {
			return -1;
		}
	} else if (datapump_uses_v22bis(datapump) &&
	    datapump->training_state == V22_TRAINING_WAIT_SCRAMBLED_MARK &&
	    datapump->has_previous_receive_point) {
		bool handled;

		if (qam_nearest_quarter_turn(
			    &datapump->previous_receive_point, &point,
			    &quarter_turns) != QAM_OK ||
		    observe_v22bis_rate_signal(
			    datapump, quarter_turns, &handled) != 0)
			return -1;
		if (datapump->training_state !=
		    V22_TRAINING_WAIT_SCRAMBLED_MARK) {
			datapump->has_previous_receive_point = false;
			return 0;
		}
		if (!handled &&
		    observe_phase_change(datapump, quarter_turns) != 0)
			return -1;
	} else if (datapump_uses_v22bis(datapump) &&
	    datapump->v22bis_receiver_initialized &&
	    v22bis_receive_window_open(datapump)) {
		if (receive_v22bis_point(datapump, &point, linked) != 0)
			return -1;
	} else if (datapump->has_previous_receive_point &&
		   qam_nearest_quarter_turn(
			   &datapump->previous_receive_point, &point,
			   &quarter_turns) == QAM_OK) {
		if ((datapump_uses_600(datapump) &&
		     observe_600_phase_change(
			     datapump,
			     &datapump->previous_receive_point,
			     &point, quarter_turns) != 0) ||
		    (!datapump_uses_600(datapump) &&
		     observe_phase_change(datapump, quarter_turns) != 0))
			return -1;
	} else if (datapump->has_previous_receive_point) {
		return -1;
	}
	if (v22bis_receive_window_open(datapump) &&
	    !datapump->v22bis_receiver_initialized) {
		if (v22bis_receiver_init(
			    &datapump->v22bis_receiver, &point) !=
		    V22BIS_RECEIVER_OK)
			return -1;
		datapump->v22bis_receiver_initialized = true;
	}

 remember_point:
	datapump->previous_receive_point = point;
	datapump->has_previous_receive_point = true;
	return 0;
}

static int enter_link(struct v22_datapump *datapump)
{
	if (modem_set_param(
		    datapump->dp.modem, MDMPRM_TX_RATE,
		    (int)datapump->bit_rate) != 0 ||
	    modem_set_param(
		    datapump->dp.modem, MDMPRM_RX_RATE,
		    (int)datapump->bit_rate) != 0)
		return -1;
	datapump->training_state = V22_TRAINING_ONLINE;
	datapump->transmit_mode = V22_TRANSMIT_DATA;
	datapump->training_samples = 0;
	datapump->phase_samples = 0;
	datapump->scrambled_mark_bits = 0;
	datapump->v22bis_confirm_bits = 0;
	datapump->rate_signal_tx_symbols = 0;
	datapump->missing_carrier_samples = 0;
	datapump->retrain_peer_samples = 0;
	datapump->retrain_retry_samples = 0;
	discard_retrain_candidate(datapump);
	datapump->retraining = false;
	datapump->retrain_gain_valid = false;
	datapump->retrain_peer_signal_received = false;
	datapump->retrain_receive_ready = false;
	v22bis_rate_signal_detector_reset(
		&datapump->rate_signal_detector);
	datapump->dp.status = DPSTAT_CONNECT;
	return 0;
}

static int advance_training_timer(
	struct v22_datapump *datapump, bool *linked)
{
	if ((datapump->transmit_mode ==
		     V22_TRANSMIT_V22BIS_RATE_SIGNAL ||
	     datapump->transmit_mode == V22_TRANSMIT_RETRAIN_SIGNAL) &&
	    datapump->rate_signal_tx_symbols >=
		    V22BIS_RATE_SIGNAL_SYMBOLS) {
		bool retrain_signal = datapump->transmit_mode ==
			V22_TRANSMIT_RETRAIN_SIGNAL;

		if (start_scrambled_transmit(datapump) != 0)
			return -1;
		if (retrain_signal) {
			datapump->training_state =
				datapump->retrain_peer_signal_received ?
					V22_TRAINING_RETRAIN_MARK :
					V22_TRAINING_RETRAIN_WAIT_PEER;
		} else if (datapump->training_state ==
		    V22_TRAINING_V22BIS_RATE_SIGNAL)
			datapump->training_state =
				V22_TRAINING_WAIT_SCRAMBLED_MARK;
	}
	if (datapump->retraining) {
		if (datapump->retrain_peer_signal_received)
			add_bounded(
				&datapump->retrain_peer_samples, 1,
				V22_RETRAIN_RETRY_SAMPLES);
		add_bounded(
			&datapump->retrain_retry_samples, 1,
			V22_RETRAIN_RETRY_SAMPLES);
		if (datapump->training_state == V22_TRAINING_RETRAIN_MARK &&
		    datapump_uses_v22bis(datapump) &&
		    datapump->retrain_peer_samples >=
			    V22BIS_TRANSMIT_START_SAMPLES &&
		    datapump->v22bis_receiver_initialized) {
			if (start_v22bis_transmit(datapump) != 0)
				return -1;
		} else if (datapump->training_state ==
			   V22_TRAINING_RETRAIN_MARK &&
			   !datapump_uses_v22bis(datapump) &&
			   datapump->retrain_peer_samples >=
				   V22_RETRAIN_CONFIRM_SAMPLES &&
			   datapump->retrain_receive_ready) {
			if (enter_link(datapump) != 0)
				return -1;
			*linked = true;
			return 0;
		}
		if (datapump->training_state ==
		    V22_TRAINING_V22BIS_CONFIRM) {
			if (datapump->phase_samples < V22BIS_CONFIRM_SAMPLES)
				datapump->phase_samples++;
			if (v22bis_is_ready_to_link(datapump)) {
				if (enter_link(datapump) != 0)
					return -1;
				*linked = true;
				return 0;
			}
		}
		return datapump->retrain_retry_samples >=
			       V22_RETRAIN_RETRY_SAMPLES &&
		       datapump->retrain_detect_symbols <
			       V22_RETRAIN_MIN_SYMBOLS ?
			       restart_retrain(datapump) : 0;
	}
	if (datapump->training_state == V22_TRAINING_ORIGINATE_SILENCE) {
		datapump->phase_samples++;
		if (datapump->phase_samples >= V22_ORIGINATE_SILENCE_SAMPLES) {
			if (datapump_uses_v22bis(datapump)) {
				if (start_v22bis_rate_signal(datapump) != 0)
					return -1;
				datapump->training_state =
					V22_TRAINING_V22BIS_RATE_SIGNAL;
			} else {
				if (start_scrambled_transmit(datapump) != 0)
					return -1;
				datapump->training_state =
					V22_TRAINING_WAIT_SCRAMBLED_MARK;
			}
			datapump->phase_samples = 0;
			datapump->scrambled_mark_bits = 0;
		}
	} else if (datapump->training_state == V22_TRAINING_CONNECT_HOLD) {
		datapump->phase_samples++;
		if (datapump_uses_v22bis(datapump) &&
		    datapump->phase_samples >=
			    V22BIS_TRANSMIT_START_SAMPLES) {
			if (start_v22bis_transmit(datapump) != 0)
				return -1;
		} else if (!datapump_uses_v22bis(datapump) &&
			   datapump->phase_samples >=
				   V22_CONNECT_HOLD_SAMPLES) {
			if (enter_link(datapump) != 0) {
				return -1;
			} else {
				*linked = true;
			}
		}
	} else if (datapump->training_state ==
		   V22_TRAINING_V22BIS_CONFIRM) {
		if (datapump->phase_samples < V22BIS_CONFIRM_SAMPLES)
			datapump->phase_samples++;
		if (v22bis_is_ready_to_link(datapump)) {
			if (enter_link(datapump) != 0)
				return -1;
			*linked = true;
		}
	}
	return 0;
}

static bool carrier_is_present(
	const struct qam_measurement *measurement,
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
	struct v22_datapump *datapump = v22_from_dp(dp);
	const int16_t *receive = receive_buffer;
	int16_t *transmit = transmit_buffer;
	struct qam_measurement measurement;
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
		    receive_sample(datapump, receive[index], &linked) != 0 ||
		    advance_training_timer(datapump, &linked) != 0) {
			memset(transmit, 0, (size_t)count * sizeof(*transmit));
			return DPSTAT_ERROR;
		}
	}
	if (linked)
		return DPSTAT_CONNECT;
	if (datapump->training_state != V22_TRAINING_ONLINE)
		return DPSTAT_OK;
	measurement = qam_demodulator_measure(
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

int v22_datapump_register(void)
{
	for (size_t index = 0; index < sizeof(v22_ids) / sizeof(v22_ids[0]);
	     ++index) {
		if (modem_dp_register(v22_ids[index], &v22_operations) == 0)
			continue;
		while (index > 0) {
			index--;
			(void)modem_dp_deregister(
				v22_ids[index], &v22_operations);
		}
		return -1;
	}
	return 0;
}

int v22_datapump_unregister(void)
{
	size_t count = sizeof(v22_ids) / sizeof(v22_ids[0]);

	for (size_t index = count; index > 0; --index) {
		if (modem_dp_deregister(
			    v22_ids[index - 1], &v22_operations) == 0)
			continue;
		for (size_t restore = index; restore < count; ++restore)
			(void)modem_dp_register(
				v22_ids[restore], &v22_operations);
		return -1;
	}
	return 0;
}

bool v22_datapump_is_busy(void)
{
	return v22_operations.use_count != 0;
}
