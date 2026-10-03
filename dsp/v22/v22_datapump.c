// SPDX-License-Identifier: BSD-3-Clause

#include "v22_datapump.h"
#include "single_tone.h"

#include "datapump_link.h"
#include "datapump_media.h"
#include "datapump_timing.h"
#include "modem.h"
#include "qam.h"
#include "v22_framing.h"
#include "v22bis_receiver.h"
#include "v22bis_symbols.h"

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
	/* Keep received data clamped while the returned carrier settles. */
	V22_CARRIER_RETURN_HOLD_SAMPLES = MODEM_RATE / 10,
	/* After 300 ms, resuming the old 2400-bit/s decisions is unsafe. */
	V22BIS_CARRIER_RETRAIN_SAMPLES = MODEM_RATE * 3 / 10,
	V22_ORIGINATE_ACQUISITION_SYMBOLS = 64,
	V22_ANSWER_ACQUISITION_SYMBOLS = 32,
	V22BIS_RECEIVE_START_SAMPLES = MODEM_RATE * 450 / 1000,
	V22BIS_TRANSMIT_START_SAMPLES = MODEM_RATE * 600 / 1000,
	V22BIS_CONFIRM_BITS = 32,
	V22BIS_CONFIRM_SAMPLES = MODEM_RATE / 5,
	/* Require a stable 80 ms suffix, but do not respond until its end. */
	V22_RETRAIN_MIN_SYMBOLS = 48,
	V22BIS_RETRAIN_DETECT_SYMBOLS = 8,
	V22BIS_RETRAIN_CONFIDENCE_Q15 = 0x7eb8,
	V22BIS_RETRAIN_MIN_REPEATED_PAIRS = 3,
	V22_RETRAIN_RETRY_SAMPLES = MODEM_RATE * 12 / 10,
};

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
	V22_TRANSMIT_BELL_ANSWER_TONE,
	V22_TRANSMIT_V22BIS_RATE_SIGNAL,
	V22_TRANSMIT_SCRAMBLED_MARK,
	V22_TRANSMIT_V22BIS_MARK,
	V22_TRANSMIT_RETRAIN_SIGNAL,
	V22_TRANSMIT_DATA,
};

struct v22_datapump {
	struct dp dp;
	struct qam_modulator modulator;
	struct single_tone_transmitter bell_answer_tone;
	struct single_tone_detector answer_mark;
	struct qam_demodulator demodulator;
	struct v22_symbol_encoder encoder;
	struct v22_symbol_decoder decoder;
	struct v22_symbol_decoder rate600_decoder;
	uint16_t rate600_confirm_bits;
	uint8_t rate600_turns;
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
	uint32_t rate_signal_wait_samples;
	uint32_t missing_carrier_samples;
	uint32_t carrier_return_hold_samples;
	uint32_t retrain_peer_samples;
	uint32_t retrain_retry_samples;
	uint16_t retrain_candidate_bit_count;
	uint32_t retrain_symbol_history;
	uint8_t retrain_candidate_bits[
		V22BIS_ALTERNATING_RUN_MAX_SYMBOLS *
		V22BIS_BITS_PER_SYMBOL];
	unsigned int bit_rate;
	uint8_t transmit_quadrant;
	uint8_t transmit_symbol;
	struct qam_point previous_receive_point;
	bool has_previous_receive_point;
	bool v22bis_receiver_initialized;
	bool retrain_peer_signal_received;
	bool retrain_candidate_deliver;
	bool rate_signal_late_candidate;
	bool rate_signal_reply_sent;
	bool originator;
};

static struct dp *create_v22(
	struct modem *modem, enum DP_ID id, int caller, int sample_rate,
	int max_fragment, const struct dp_operations *operations);
static int delete_v22(struct dp *dp);
static int process_v22(
	struct dp *dp, void *receive, void *transmit, int count);
static int retrain_v22(struct dp *dp);
static const char *progress_v22(const struct dp *dp)
{
	const struct v22_datapump *datapump = dp->dp_data;

	if (datapump == NULL)
		return "V.22/initializing";
	switch (datapump->training_state) {
	case V22_TRAINING_ORIGINATE_WAIT_ANSWER_MARK: return "V.22/wait-answer-mark";
	case V22_TRAINING_ORIGINATE_SILENCE: return "V.22/answer-mark-guard";
	case V22_TRAINING_V22BIS_RATE_SIGNAL: return "V.22bis/send-S1";
	case V22_TRAINING_WAIT_SCRAMBLED_MARK: return "V.22/wait-scrambled-mark";
	case V22_TRAINING_CONNECT_HOLD: return "V.22/mark-exchange";
	case V22_TRAINING_V22BIS_CONFIRM: return "V.22bis/confirm-2400";
	case V22_TRAINING_RETRAIN_SIGNAL: return "V.22bis/retrain-S1";
	case V22_TRAINING_RETRAIN_WAIT_PEER: return "V.22bis/retrain-wait-peer";
	case V22_TRAINING_RETRAIN_MARK: return "V.22bis/retrain-mark";
	case V22_TRAINING_ONLINE: return "online";
	}
	return "V.22/unknown";
}

static int enter_link(struct v22_datapump *datapump);
static int flush_retrain_candidate(struct v22_datapump *datapump);

/* V.22 6.3.1.2 keeps the established carrier instead of repeating a
 * handshake.  Bell 212A has the same carrier-preserving ATO1 behavior, so
 * only V.22bis advertises the reciprocal retrain operation. */
static const struct dp_operations v22_operations = {
	.name = "V.22/Bell 212A",
	.create = create_v22,
	.delete = delete_v22,
	.process = process_v22,
	.progress = progress_v22,
};

static const struct dp_operations v22bis_operations = {
	.name = "V.22bis",
	.create = create_v22,
	.delete = delete_v22,
	.process = process_v22,
	.retrain = retrain_v22,
	.progress = progress_v22,
};

const struct dp_operations *v22_datapump_operations(enum DP_ID id)
{
	if (id == DP_V22BIS)
		return &v22bis_operations;
	if (id == DP_V22 || id == DP_B212)
		return &v22_operations;
	return NULL;
}

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

static bool datapump_is_retraining(
	const struct v22_datapump *datapump)
{
	if (datapump == NULL)
		return false;
	if (datapump->training_state == V22_TRAINING_RETRAIN_SIGNAL ||
	    datapump->training_state == V22_TRAINING_RETRAIN_WAIT_PEER ||
	    datapump->training_state == V22_TRAINING_RETRAIN_MARK)
		return true;
	return datapump->training_state == V22_TRAINING_V22BIS_CONFIRM &&
	       datapump->retrain_peer_signal_received;
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
	    dp->op != v22_datapump_operations(dp->id) ||
	    dp->modem == NULL || dp->dp_data == NULL)
		return NULL;
	datapump = dp->dp_data;
	if (&datapump->dp != dp ||
	    !bit_rate_matches_id(datapump->dp.id, datapump->bit_rate) ||
	    datapump->training_state > V22_TRAINING_ONLINE ||
	    datapump->transmit_mode > V22_TRANSMIT_DATA ||
	    (datapump->originator && !single_tone_detector_is_valid(&datapump->answer_mark)) ||
	    (datapump->transmit_mode == V22_TRANSMIT_BELL_ANSWER_TONE &&
	     (dp->id != DP_B212 || datapump->originator ||
	      !single_tone_transmitter_is_valid(&datapump->bell_answer_tone))) ||
	    datapump->transmit_quadrant > 3 ||
	    datapump->training_timeout_samples == 0 ||
	    datapump->training_samples > datapump->training_timeout_samples ||
	    datapump->phase_samples > V22_CONNECT_HOLD_SAMPLES ||
	    datapump->answer_mark_symbols > V22_ANSWER_MARK_CONFIRM_SYMBOLS ||
	    datapump->scrambled_mark_bits >
		    V22_ORIGINATE_SCRAMBLED_CONFIRM_BITS ||
	    datapump->v22bis_confirm_bits > V22BIS_CONFIRM_BITS ||
	    datapump->rate600_confirm_bits > V22_ORIGINATE_SCRAMBLED_CONFIRM_BITS / 2 ||
	    (datapump->rate600_turns & ~0xau) != 0 ||
	    datapump->rate600_decoder.phase_quadrant >= QPSK_QUADRANT_COUNT ||
	    !v22_scrambler_is_valid(&datapump->rate600_decoder.descrambler) ||
	    datapump->rate_signal_tx_symbols >
		    V22BIS_RATE_SIGNAL_SYMBOLS ||
	    datapump->rate_signal_wait_samples >
		    V22_RETRAIN_RETRY_SAMPLES ||
	    datapump->retrain_candidate_bit_count >
		    sizeof(datapump->retrain_candidate_bits) ||
	    datapump->retrain_peer_samples > V22_RETRAIN_RETRY_SAMPLES ||
	    datapump->retrain_retry_samples > V22_RETRAIN_RETRY_SAMPLES ||
	    datapump->missing_carrier_samples >
		    datapump_carrier_loss_ticks(datapump->dp.modem) ||
	    datapump->carrier_return_hold_samples >
		    V22_CARRIER_RETURN_HOLD_SAMPLES ||
	    (datapump->carrier_return_hold_samples != 0 &&
	     (datapump->training_state != V22_TRAINING_ONLINE ||
	      datapump->missing_carrier_samples != 0)) ||
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
	    (datapump->v22bis_receiver_initialized &&
	     (!datapump->has_previous_receive_point ||
	      !v22bis_receiver_is_valid(
		      &datapump->v22bis_receiver))) ||
	    (transmit_uses_v22bis_symbols(datapump) &&
	     (datapump->v22bis_encoder.phase_quadrant >=
		      QPSK_QUADRANT_COUNT ||
	      !v22_scrambler_is_valid(
		      &datapump->v22bis_encoder.scrambler))) ||
	    ((datapump->rate_signal_detector.present ||
	      datapump->rate_signal_detector.alternating_symbols != 0) &&
	     (!datapump_uses_v22bis(datapump) ||
	      (datapump->training_state != V22_TRAINING_WAIT_SCRAMBLED_MARK &&
	       datapump->training_state != V22_TRAINING_ONLINE &&
	       !datapump_is_retraining(datapump)))) ||
	    (datapump->training_state != V22_TRAINING_WAIT_SCRAMBLED_MARK &&
	     (datapump->rate_signal_wait_samples != 0 ||
	      datapump->rate_signal_late_candidate ||
	      datapump->rate_signal_reply_sent)) ||
	    (datapump->rate_signal_late_candidate &&
	     (!datapump->originator ||
	      !datapump->rate_signal_detector.present ||
	      datapump->rate_signal_wait_samples <
		      V22_RETRAIN_RETRY_SAMPLES)) ||
	    (datapump->rate_signal_reply_sent &&
	     !datapump->rate_signal_late_candidate) ||
	    (!datapump_uses_v22bis(datapump) &&
	     (datapump->v22bis_receiver_initialized ||
	      datapump->v22bis_confirm_bits != 0 ||
	      datapump->transmit_mode == V22_TRANSMIT_V22BIS_RATE_SIGNAL ||
	      datapump->transmit_mode == V22_TRANSMIT_V22BIS_MARK)) ||
	    (datapump_is_retraining(datapump) &&
	     !datapump_uses_v22bis(datapump)) ||
	    (!datapump_is_retraining(datapump) &&
	     (datapump->retrain_peer_signal_received ||
	      datapump->retrain_peer_samples != 0 ||
	      datapump->retrain_retry_samples != 0)) ||
	    (datapump->retrain_candidate_bit_count == 0 &&
	     datapump->retrain_candidate_deliver) ||
	    (datapump->training_state == V22_TRAINING_RETRAIN_WAIT_PEER &&
	     datapump->retrain_peer_signal_received) ||
	    ((datapump->training_state == V22_TRAINING_RETRAIN_MARK ||
	      (datapump_is_retraining(datapump) &&
	       datapump->training_state == V22_TRAINING_V22BIS_CONFIRM)) &&
	     !datapump->retrain_peer_signal_received) ||
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
	int max_fragment, const struct dp_operations *operations)
{
	struct v22_datapump *datapump;
	const struct qam_channel *transmit_channel;
	const struct qam_channel *receive_channel;
	unsigned int bit_rate = selected_bit_rate(modem, id);

	if (modem == NULL || !id_uses_v22_carrier(id) ||
	    (caller != 0 && caller != 1) ||
	    sample_rate != MODEM_RATE || max_fragment != MODEM_FRAG ||
	    operations != v22_datapump_operations(id) ||
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
	/* Initial timing recovery is part of V.22's 155 +/- 50 ms originate
	 * detection and 270 +/- 40 ms answer response intervals.  The generic
	 * 256-symbol window alone exceeds both. */
	if (qam_modulator_init(
		    &datapump->modulator, transmit_channel,
		    V22_TRANSMIT_AMPLITUDE) != QAM_OK ||
	    qam_demodulator_init_with_timing_windows(
		    &datapump->demodulator, receive_channel,
		    datapump->originator ?
			    V22_ORIGINATE_ACQUISITION_SYMBOLS :
			    V22_ANSWER_ACQUISITION_SYMBOLS,
		    QAM_DEFAULT_TIMING_WINDOW_SYMBOLS) != QAM_OK ||
	    v22_symbol_encoder_init(&datapump->encoder, 0) !=
		    V22_FRAMING_OK ||
	    v22_symbol_decoder_init(&datapump->decoder, 0) !=
		    V22_FRAMING_OK) {
		free(datapump);
		return NULL;
	}
	datapump->rate600_decoder = datapump->decoder;
	if (id == DP_B212 && !datapump->originator &&
	    single_tone_transmitter_init(&datapump->bell_answer_tone, 2225,
		    V22_TRANSMIT_AMPLITUDE) != SINGLE_TONE_OK) {
		free(datapump);
		return NULL;
	}
	if (datapump->originator &&
	    single_tone_detector_init(&datapump->answer_mark, 2250) != SINGLE_TONE_OK) {
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
		datapump->transmit_mode = id == DP_B212 ?
			V22_TRANSMIT_BELL_ANSWER_TONE : V22_TRANSMIT_UNSCRAMBLED_MARK;
	}
	return &datapump->dp;
}

static int delete_v22(struct dp *dp)
{
	struct v22_datapump *datapump = v22_from_dp(dp);

	if (datapump == NULL)
		return -1;
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
	uint8_t bits[2];
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
	if (!datapump_source_bits(
		    &datapump->dp,
		    datapump->transmit_mode == V22_TRANSMIT_DATA,
		    bits, bit_count))
		return -1;
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
	uint8_t bits[V22BIS_BITS_PER_SYMBOL];

	if (!datapump_source_bits(
		    &datapump->dp,
		    datapump->transmit_mode == V22_TRANSMIT_DATA,
		    bits, V22BIS_BITS_PER_SYMBOL))
		return -1;
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
	if (datapump->transmit_mode == V22_TRANSMIT_BELL_ANSWER_TONE)
		return single_tone_transmitter_sample(&datapump->bell_answer_tone,
			sample) == SINGLE_TONE_OK ? 0 : -1;
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
	struct v22_symbol_decoder *decoder,
	const struct qam_point *previous,
	const struct qam_point *current, uint8_t *bit, uint8_t *quarter_turns)
{
	double cross;
	uint8_t quadrant;

	if (!point_is_finite(previous) || !point_is_finite(current) ||
	    decoder == NULL || bit == NULL || quarter_turns == NULL)
		return -1;
	cross = previous->in_phase * current->quadrature -
		previous->quadrature * current->in_phase;
	if (!isfinite(cross))
		return -1;
	/* Choose between the two Table 1 rotations before quarter-turn
	 * quantization discards which side of a tie the observation occupied. */
	*quarter_turns = cross < 0.0 ? 3u : 1u;
	quadrant = (uint8_t)(
		(decoder->phase_quadrant + *quarter_turns) & 3u);
	return v22_600_symbol_decode(
		       decoder, quadrant, bit) == V22_FRAMING_OK ?
		       0 : -1;
}

static uint32_t scrambled_confirmation_target(
	const struct v22_datapump *datapump)
{
	uint32_t bits = datapump->originator ? V22_ORIGINATE_SCRAMBLED_CONFIRM_BITS :
		V22_ANSWER_SCRAMBLED_CONFIRM_BITS;

	/* The confirmation interval is measured in time at either bit rate. */
	return datapump_uses_600(datapump) ? bits / 2 : bits;
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

/* Recognize scrambled 600-bit/s ones in the same acquired QAM points.
 * Requiring both Table 1 rotations excludes the constant answer mark. */
static int observe_auto_600(struct v22_datapump *datapump,
	const struct qam_point *point, bool *selected)
{
	uint8_t bit, turns;
	uint32_t target = scrambled_confirmation_target(datapump) / 2;

	*selected = false;
	if (!MODEM_AUTOMODE(datapump->dp.modem) ||
	    datapump->dp.modem->min_rate > V22_600_BIT_RATE ||
	    datapump->dp.modem->max_rate < V22_600_BIT_RATE ||
	    datapump->dp.id == DP_B212 || datapump_uses_600(datapump) ||
	    !datapump->has_previous_receive_point ||
	    (datapump->training_state != V22_TRAINING_WAIT_SCRAMBLED_MARK &&
	     datapump->training_state != V22_TRAINING_CONNECT_HOLD))
		return 0;
	if (decode_600_phase_change(&datapump->rate600_decoder,
		    &datapump->previous_receive_point, point, &bit, &turns) != 0)
		return -1;
	if (bit == 0) {
		datapump->rate600_confirm_bits = 0;
		datapump->rate600_turns = 0;
		return 0;
	}
	if (datapump->rate600_confirm_bits < target)
		datapump->rate600_confirm_bits++;
	datapump->rate600_turns |= (uint8_t)(1u << turns);
	if (datapump->rate600_confirm_bits < target ||
	    datapump->rate600_turns != 0xau)
		return 0;
	datapump->bit_rate = V22_600_BIT_RATE;
	datapump->dp.id = DP_V22;
	datapump->dp.op = &v22_operations;
	datapump->decoder = datapump->rate600_decoder;
	datapump->v22bis_receiver_initialized = false;
	datapump->v22bis_confirm_bits = 0;
	datapump->rate_signal_tx_symbols = 0;
	datapump->rate_signal_wait_samples = 0;
	datapump->rate_signal_late_candidate = false;
	datapump->rate_signal_reply_sent = false;
	v22bis_rate_signal_detector_reset(&datapump->rate_signal_detector);
	if (datapump->originator && start_scrambled_transmit(datapump) != 0)
		return -1;
	*selected = true;
	return begin_connect_hold(datapump);
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
		if (quarter_turns == 3 && datapump->answer_mark.tone_present)
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

static int deliver_received_bits(
	struct v22_datapump *datapump, uint8_t *bits,
	size_t bit_count)
{
	struct modem *modem = datapump->dp.modem;

	if (modem->put_bits == NULL)
		return 0;
	return modem_put_bits(modem, 1, bits, (int)bit_count) ==
		       (int)bit_count ? 0 : -1;
}

static int output_received_bits(
	struct v22_datapump *datapump, uint8_t *bits,
	size_t bit_count)
{
	if (datapump->rate_signal_detector.alternating_symbols != 0) {
		if (datapump->training_state == V22_TRAINING_ONLINE &&
		    datapump->carrier_return_hold_samples == 0 &&
		    !datapump->retrain_candidate_deliver) {
			datapump->retrain_candidate_bit_count = 0;
			datapump->retrain_candidate_deliver = true;
		}
		if (bit_count >
		    sizeof(datapump->retrain_candidate_bits) -
			    datapump->retrain_candidate_bit_count)
			return -1;
		memcpy(datapump->retrain_candidate_bits +
			       datapump->retrain_candidate_bit_count,
		       bits, bit_count);
		datapump->retrain_candidate_bit_count += (uint16_t)bit_count;
	} else if (datapump->carrier_return_hold_samples == 0)
		return deliver_received_bits(datapump, bits, bit_count);
	return 0;
}

static int observe_bits(
	struct v22_datapump *datapump, uint8_t *bits,
	size_t bit_count)
{
	if ((datapump->training_state ==
		     V22_TRAINING_WAIT_SCRAMBLED_MARK ||
	     datapump->training_state == V22_TRAINING_CONNECT_HOLD) &&
	    (!datapump_uses_v22bis(datapump) ||
	     (datapump->training_state == V22_TRAINING_WAIT_SCRAMBLED_MARK &&
	      MODEM_AUTOMODE(datapump->dp.modem) &&
	      datapump->dp.modem->min_rate <= V22_1200_BIT_RATE &&
	      datapump->dp.modem->max_rate >= V22_1200_BIT_RATE))) {
		uint32_t target = scrambled_confirmation_target(datapump);

		for (size_t index = 0; index < bit_count; ++index) {
			if (bits[index] == 1 &&
			    datapump->scrambled_mark_bits < target)
				datapump->scrambled_mark_bits++;
			else if (bits[index] == 0)
				datapump->scrambled_mark_bits = 0;
			if (datapump->training_state ==
				    V22_TRAINING_WAIT_SCRAMBLED_MARK &&
			    datapump->scrambled_mark_bits >= target) {
				/* V.22bis 6.3.1.1: scrambled ones without S1
				 * select 1200 bit/s. Retain the acquired carrier,
				 * descrambler and transmit history. */
				if (datapump_uses_v22bis(datapump)) {
					datapump->bit_rate = V22_1200_BIT_RATE;
					datapump->dp.id = DP_V22;
					datapump->dp.op = &v22_operations;
					datapump->rate_signal_wait_samples = 0;
					datapump->rate_signal_late_candidate = false;
					datapump->rate_signal_reply_sent = false;
					v22bis_rate_signal_detector_reset(
						&datapump->rate_signal_detector);
				}
				return begin_connect_hold(datapump);
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
	if (decode_600_phase_change(&datapump->decoder, previous, current,
		    &bit, &quarter_turns) != 0)
		return -1;
	return observe_bits(datapump, &bit, 1);
}

static int begin_v22bis_transition(
	struct v22_datapump *datapump, bool reciprocal_rate_signal)
{
	if (!datapump_uses_v22bis(datapump) ||
	    datapump->training_state != V22_TRAINING_WAIT_SCRAMBLED_MARK ||
	    datapump->v22bis_receiver_initialized ||
	    (reciprocal_rate_signal &&
	     (!datapump->originator ||
	      datapump->rate_signal_tx_symbols <
		      V22BIS_RATE_SIGNAL_MIN_SYMBOLS ||
	      (datapump->transmit_mode !=
		       V22_TRANSMIT_V22BIS_RATE_SIGNAL &&
	       (datapump->transmit_mode != V22_TRANSMIT_SCRAMBLED_MARK ||
		datapump->rate_signal_tx_symbols !=
			V22BIS_RATE_SIGNAL_SYMBOLS)))))
		return -1;
	if (datapump->originator &&
	    !reciprocal_rate_signal &&
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
	datapump->rate_signal_wait_samples = 0;
	datapump->rate_signal_late_candidate = false;
	datapump->rate_signal_reply_sent = false;
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
	    datapump->training_state != V22_TRAINING_WAIT_SCRAMBLED_MARK)
		return -1;
	if (v22bis_rate_signal_observe(
		    &datapump->rate_signal_detector, quarter_turns,
		    &event) != V22_FRAMING_OK)
		return -1;
	*handled = false;
	if (event == V22BIS_RATE_SIGNAL_STARTED) {
		/* A retrying peer decides at its S1 boundary whether we replied.
		 * Start after the stable prefix, then require the complete S1 before
		 * advancing our own transition clocks. */
		datapump->rate_signal_late_candidate =
			datapump->originator &&
			datapump->rate_signal_wait_samples >=
				V22_RETRAIN_RETRY_SAMPLES;
		datapump->rate_signal_reply_sent = false;
		if (datapump->rate_signal_late_candidate &&
		    datapump->transmit_mode ==
			    V22_TRANSMIT_SCRAMBLED_MARK) {
			if (start_v22bis_rate_signal(datapump) != 0)
				return -1;
			datapump->rate_signal_reply_sent = true;
		}
	}
	if (event == V22BIS_RATE_SIGNAL_ENDED ||
	    event == V22BIS_RATE_SIGNAL_FULLY_ENDED) {
		bool accept = !datapump->rate_signal_late_candidate ||
			(datapump->rate_signal_reply_sent &&
			 event == V22BIS_RATE_SIGNAL_FULLY_ENDED);
		bool reciprocal = datapump->rate_signal_reply_sent;

		datapump->rate_signal_late_candidate = false;
		datapump->rate_signal_reply_sent = false;
		if (!accept)
			return 0;
		phase = datapump->decoder.phase_quadrant;
		if (v22_symbol_decoder_init(
			    &datapump->decoder, phase) != V22_FRAMING_OK ||
		    begin_v22bis_transition(datapump, reciprocal) != 0)
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
	       datapump->v22bis_confirm_bits >= V22BIS_CONFIRM_BITS;
}

static int observe_v22bis_bits(
	struct v22_datapump *datapump,
	uint8_t bits[V22BIS_BITS_PER_SYMBOL], bool *linked)
{
	if (datapump->training_state == V22_TRAINING_ONLINE)
		return output_received_bits(
			datapump, bits, V22BIS_BITS_PER_SYMBOL);
	if (datapump_is_retraining(datapump) &&
	    datapump->v22bis_confirm_bits >= V22BIS_CONFIRM_BITS) {
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

	if (!datapump->v22bis_receiver.v22bis_started) {
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
	return observe_v22bis_bits(datapump, bits, linked);
}

static bool v22bis_receive_window_open(
	const struct v22_datapump *datapump)
{
	if (!datapump_uses_v22bis(datapump))
		return false;
	if (datapump_is_retraining(datapump))
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

static int flush_retrain_candidate(
	struct v22_datapump *datapump)
{
	if (datapump->retrain_candidate_bit_count != 0 &&
	    datapump->retrain_candidate_deliver &&
	    deliver_received_bits(
		    datapump, datapump->retrain_candidate_bits,
		    datapump->retrain_candidate_bit_count) != 0)
		return -1;
	datapump->retrain_candidate_bit_count = 0;
	datapump->retrain_candidate_deliver = false;
	return 0;
}

static void discard_retrain_candidate(
	struct v22_datapump *datapump)
{
	v22bis_rate_signal_detector_reset(
		&datapump->rate_signal_detector);
	datapump->retrain_candidate_bit_count = 0;
	datapump->retrain_symbol_history = 0;
	datapump->retrain_candidate_deliver = false;
	datapump->rate_signal_late_candidate = false;
	datapump->rate_signal_reply_sent = false;
}

static int shift_retrain_candidate(struct v22_datapump *datapump)
{
	size_t bit_count = V22BIS_BITS_PER_SYMBOL;

	if (!datapump_uses_v22bis(datapump))
		return -1;
	if (datapump->retrain_candidate_bit_count < bit_count)
		return datapump->retrain_candidate_deliver ? -1 : 0;
	if (datapump->retrain_candidate_deliver &&
	    deliver_received_bits(
		    datapump, datapump->retrain_candidate_bits,
		    bit_count) != 0)
		return -1;
	datapump->retrain_candidate_bit_count -= (uint16_t)bit_count;
	memmove(datapump->retrain_candidate_bits,
		datapump->retrain_candidate_bits + bit_count,
		datapump->retrain_candidate_bit_count);
	return 0;
}

static bool v22bis_retrain_sequence_present(
	uint32_t history, size_t observed_symbols)
{
	uint8_t symbols[V22BIS_RETRAIN_DETECT_SYMBOLS];
	int square_sum = 0;
	int weighted_sum = 0;
	int weight_total = 0;
	unsigned int repeated_pairs = 0;
	int difference;
	int magnitude;
	int threshold;

	if (observed_symbols < V22BIS_RETRAIN_DETECT_SYMBOLS)
		return false;
	for (size_t remaining = V22BIS_RETRAIN_DETECT_SYMBOLS;
	     remaining != 0; --remaining) {
		symbols[remaining - 1u] = (uint8_t)(history & 15u);
		history >>= 4;
	}
	for (size_t index = 0;
	     index < V22BIS_RETRAIN_DETECT_SYMBOLS; ++index) {
		unsigned int quadrant = (symbols[index] >> 2) & 3u;

		square_sum += (int)(quadrant * quadrant);
	}
	for (size_t index = 0;
	     index < V22BIS_RETRAIN_DETECT_SYMBOLS; index += 2u) {
		unsigned int quadrant = (symbols[index] >> 2) & 3u;

		weighted_sum += (int)(quadrant * 3u);
		weight_total += 9;
		if (index != 0 &&
		    index + 1u < V22BIS_RETRAIN_DETECT_SYMBOLS) {
			if (symbols[index] == symbols[index - 2u] &&
			    symbols[index + 1u] == symbols[index - 1u])
				repeated_pairs++;
			else
				repeated_pairs = 0;
		}
	}
	difference = weighted_sum * 2 - square_sum;
	magnitude = difference < 0 ? -difference : difference;
	threshold =
		(weight_total * V22BIS_RETRAIN_CONFIDENCE_Q15) >> 15;
	return magnitude >= threshold &&
	       repeated_pairs >= V22BIS_RETRAIN_MIN_REPEATED_PAIRS;
}

static uint8_t v22_retrain_phase_label(uint8_t quarter_turns)
{
	/* V.22 Table 1 maps differential turns 0, 1, 2, 3 to the
	 * phase labels 1, 0, 2, 3 used by the sequence detector. */
	return quarter_turns < 2u ? (uint8_t)(quarter_turns ^ 1u) :
				    quarter_turns;
}

static int v22bis_hard_symbol(
	const struct v22_datapump *datapump,
	const struct qam_point *point, uint8_t *symbol)
{
	double best_error = HUGE_VAL;
	uint8_t best_symbol = 0;

	if (!datapump_uses_v22bis(datapump) || point == NULL ||
	    !point_is_finite(point) || symbol == NULL ||
	    !datapump->v22bis_receiver_initialized ||
	    !v22bis_receiver_is_valid(&datapump->v22bis_receiver))
		return -1;
	for (uint8_t candidate = 0; candidate < V22BIS_SYMBOL_COUNT;
	     ++candidate) {
		struct qam_point ideal;
		struct qam_point predicted;
		double in_phase_error;
		double quadrature_error;
		double error;

		if (v22bis_constellation_point(candidate, &ideal) !=
			    V22_FRAMING_OK ||
		    qam_gain_predict(
			    &datapump->v22bis_receiver.gain, &ideal,
			    &predicted) != QAM_GAIN_OK)
			return -1;
		in_phase_error = point->in_phase - predicted.in_phase;
		quadrature_error =
			point->quadrature - predicted.quadrature;
		error = in_phase_error * in_phase_error +
			quadrature_error * quadrature_error;
		if (isfinite(error) && error < best_error) {
			best_error = error;
			best_symbol = candidate;
		}
	}
	if (!isfinite(best_error))
		return -1;
	*symbol = best_symbol;
	return 0;
}

static int remember_retrain_symbol(
	struct v22_datapump *datapump, uint8_t symbol,
	bool discard_oldest)
{
	if (!datapump_uses_v22bis(datapump) ||
	    symbol >= V22BIS_SYMBOL_COUNT)
		return -1;
	if (discard_oldest) {
		if (shift_retrain_candidate(datapump) != 0)
			return -1;
	}
	datapump->retrain_symbol_history =
		(datapump->retrain_symbol_history << 4) | symbol;
	return 0;
}

static int observe_retrain_signal(
	struct v22_datapump *datapump, uint8_t quarter_turns,
	uint8_t radial_bits, bool online, bool *ended,
	uint8_t *current_quadrant)
{
	enum v22bis_rate_signal_event event;
	uint8_t previous_symbols;
	bool continued;
	uint8_t quadrant;
	uint8_t phase_label = v22_retrain_phase_label(quarter_turns);
	uint8_t symbol = (uint8_t)((phase_label << 2) | radial_bits);

	if (!datapump_uses_v22bis(datapump) || radial_bits >= 4u ||
	    ended == NULL || current_quadrant == NULL)
		return -1;
	*ended = false;
	quadrant = (uint8_t)(
		(receive_phase_quadrant(datapump) + quarter_turns) & 3u);
	*current_quadrant = quadrant;
	previous_symbols =
		datapump->rate_signal_detector.alternating_symbols;
	if (v22bis_rate_signal_observe(
		    &datapump->rate_signal_detector, quarter_turns,
		    &event) != V22_FRAMING_OK)
		return -1;
	continued = previous_symbols != 0 &&
		datapump->rate_signal_detector.alternating_symbols > 1u;
	if (!continued) {
		if (previous_symbols >=
			    V22_RETRAIN_MIN_SYMBOLS &&
		    (!online ||
		     v22bis_retrain_sequence_present(
			     datapump->retrain_symbol_history,
			     previous_symbols))) {
			discard_retrain_candidate(datapump);
			*ended = true;
			return 0;
		}
		if (flush_retrain_candidate(datapump) != 0)
			return -1;
		datapump->retrain_symbol_history = 0;
		if (datapump->rate_signal_detector.alternating_symbols != 0) {
			if (remember_retrain_symbol(
				    datapump, symbol, false) != 0)
				return -1;
			datapump->retrain_candidate_deliver =
				(online || datapump->v22bis_confirm_bits >=
					 V22BIS_CONFIRM_BITS) &&
				datapump->carrier_return_hold_samples == 0;
		}
	} else {
		if (remember_retrain_symbol(
			    datapump, symbol,
			    previous_symbols >=
				    V22BIS_ALTERNATING_RUN_MAX_SYMBOLS) != 0)
			return -1;
	}
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

static int reset_v22_receive_path(struct v22_datapump *datapump)
{
	struct qam_demodulator demodulator;
	struct v22_symbol_decoder decoder;

	if (datapump == NULL || !id_uses_v22_carrier(datapump->dp.id))
		return -1;
	demodulator = datapump->demodulator;
	if (qam_demodulator_reset(&demodulator) != QAM_OK ||
	    v22_symbol_decoder_init(&decoder, 0) != V22_FRAMING_OK)
		return -1;
	datapump->demodulator = demodulator;
	datapump->decoder = decoder;
	datapump->rate600_decoder = decoder;
	datapump->rate600_confirm_bits = 0;
	datapump->rate600_turns = 0;
	datapump->previous_receive_point = (struct qam_point){ 0 };
	datapump->has_previous_receive_point = false;
	datapump->v22bis_receiver_initialized = false;
	return 0;
}

static void reset_retrain_receive_progress(
	struct v22_datapump *datapump)
{
	datapump->scrambled_mark_bits = 0;
	datapump->v22bis_confirm_bits = 0;
	datapump->retrain_peer_samples = 0;
	discard_retrain_candidate(datapump);
	datapump->v22bis_receiver_initialized = false;
}

static int begin_retrain(
	struct v22_datapump *datapump, uint8_t receive_quadrant,
	bool peer_signal_received)
{
	if (datapump == NULL || !datapump_uses_v22bis(datapump) ||
	    datapump->training_state != V22_TRAINING_ONLINE ||
	    receive_quadrant >= QPSK_QUADRANT_COUNT)
		return -1;
	if (peer_signal_received) {
		if (reset_v22_receive_path(datapump) != 0)
			return -1;
	} else if (v22_symbol_decoder_init(
			   &datapump->decoder, receive_quadrant) !=
		   V22_FRAMING_OK) {
		return -1;
	}
	datapump->training_samples = 0;
	datapump->phase_samples = 0;
	datapump->missing_carrier_samples = 0;
	datapump->carrier_return_hold_samples = 0;
	reset_retrain_receive_progress(datapump);
	datapump->retrain_peer_signal_received = peer_signal_received;
	/* Keep queued transmit framing, but discard a partial receive character
	 * across the physical-layer discontinuity.  LAPM owns its own state. */
	if (!datapump->dp.modem->cfg.ec)
		modem_async_retrain(datapump->dp.modem);
	if (start_retrain_signal(datapump) != 0)
		return -1;
	datapump->dp.status = DPSTAT_OK;
	return 0;
}

static int restart_retrain(struct v22_datapump *datapump)
{
	uint8_t receive_quadrant = receive_phase_quadrant(datapump);

	if (!datapump_is_retraining(datapump) ||
	    !datapump_uses_v22bis(datapump) ||
	    v22_symbol_decoder_init(
		    &datapump->decoder, receive_quadrant) != V22_FRAMING_OK)
		return -1;
	datapump->phase_samples = 0;
	reset_retrain_receive_progress(datapump);
	datapump->retrain_peer_signal_received = false;
	if (!datapump->dp.modem->cfg.ec)
		modem_async_retrain(datapump->dp.modem);
	return start_retrain_signal(datapump);
}

static int note_peer_retrain_signal(
	struct v22_datapump *datapump)
{
	if (!datapump_is_retraining(datapump) ||
	    datapump->retrain_peer_signal_received ||
	    (datapump->training_state != V22_TRAINING_RETRAIN_WAIT_PEER &&
	     datapump->training_state != V22_TRAINING_RETRAIN_SIGNAL))
		return -1;
	if (reset_v22_receive_path(datapump) != 0)
		return -1;
	datapump->retrain_peer_signal_received = true;
	datapump->retrain_retry_samples = 0;
	reset_retrain_receive_progress(datapump);
	if (datapump->training_state == V22_TRAINING_RETRAIN_WAIT_PEER)
		datapump->training_state = V22_TRAINING_RETRAIN_MARK;
	return 0;
}

static int repeat_peer_retrain_signal(
	struct v22_datapump *datapump)
{
	if (!datapump_is_retraining(datapump) ||
	    !datapump_uses_v22bis(datapump) ||
	    !datapump->retrain_peer_signal_received)
		return -1;
	if (reset_v22_receive_path(datapump) != 0)
		return -1;
	datapump->phase_samples = 0;
	reset_retrain_receive_progress(datapump);
	if (!datapump->dp.modem->cfg.ec)
		modem_async_retrain(datapump->dp.modem);
	return start_retrain_signal(datapump);
}

static int retrain_v22(struct dp *dp)
{
	struct v22_datapump *datapump = v22_from_dp(dp);

	if (datapump == NULL || !datapump_uses_v22bis(datapump) ||
	    datapump->training_state != V22_TRAINING_ONLINE)
		return -1;
	if (flush_retrain_candidate(datapump) != 0)
		return -1;
	return begin_retrain(
		datapump, receive_phase_quadrant(datapump), false);
}

static int detect_online_retrain(
	struct v22_datapump *datapump, const struct qam_point *point,
	bool *started)
{
	uint8_t current_quadrant;
	uint8_t quarter_turns;
	uint8_t radial_bits;
	uint8_t absolute_symbol;
	uint8_t previous_quadrant;
	bool ended;

	if (!datapump_uses_v22bis(datapump) || started == NULL)
		return -1;
	*started = false;
	if (!datapump->has_previous_receive_point)
		return 0;
	if (v22bis_hard_symbol(datapump, point, &absolute_symbol) != 0)
		return -1;
	previous_quadrant =
		datapump->v22bis_receiver.decoder.phase_quadrant;
	quarter_turns = (uint8_t)(
		((absolute_symbol >> 2) - previous_quadrant) & 3u);
	radial_bits = (uint8_t)(absolute_symbol & 3u);
	if (observe_retrain_signal(
		    datapump, quarter_turns, radial_bits, true, &ended,
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

	if (!datapump_uses_v22bis(datapump) ||
	    !datapump->has_previous_receive_point || ended == NULL)
		return -1;
	if (qam_nearest_quarter_turn(
		    &datapump->previous_receive_point, point,
		    &quarter_turns) != QAM_OK ||
	    observe_retrain_signal(
		    datapump, quarter_turns, 0, false, ended,
		    &current_quadrant) != 0)
		return -1;
	if (*ended && !datapump->retrain_peer_signal_received)
		return note_peer_retrain_signal(datapump);
	if (*ended) {
		/* A peer keeps transmitting S1 after it is first recognized.  Ignore
		 * that continuation during the S1/mark exchange.  Once 2400-bit/s
		 * confirmation has begun, a new S1 proves the peer did not follow and
		 * must restart the bounded exchange. */
		if (datapump->training_state == V22_TRAINING_V22BIS_CONFIRM)
			return repeat_peer_retrain_signal(datapump);
		return 0;
	}
	if (!decode_current)
		datapump->decoder.phase_quadrant = current_quadrant;
	return 0;
}

static int receive_sample(
	struct v22_datapump *datapump, int16_t sample, bool discard,
	bool *linked)
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
	if (discard)
		goto remember_point;
	{
		bool selected;

		if (observe_auto_600(datapump, &point, &selected) != 0)
			return -1;
		if (selected)
			goto remember_point;
	}

	if (datapump_is_retraining(datapump)) {
		bool decode_current =
			datapump->retrain_peer_signal_received;
		bool ended;

		if (!datapump->has_previous_receive_point)
			goto remember_point;
		if (detect_peer_retrain(
			    datapump, &point, decode_current, &ended) != 0)
			return -1;
		if (!datapump->has_previous_receive_point)
			return 0;
		if (ended || !decode_current)
			goto remember_point;
		if (datapump->v22bis_receiver_initialized &&
			   v22bis_receive_window_open(datapump)) {
			if (receive_v22bis_point(
				    datapump, &point, linked) != 0)
				return -1;
		} else if (datapump->has_previous_receive_point &&
			   qam_nearest_quarter_turn(
				   &datapump->previous_receive_point, &point,
				   &quarter_turns) == QAM_OK) {
			if (observe_phase_change(datapump, quarter_turns) != 0)
				return -1;
		} else if (datapump->has_previous_receive_point) {
			return -1;
		}
	} else if (datapump->training_state == V22_TRAINING_ONLINE) {
		bool retrain_started = false;

		if (datapump_uses_v22bis(datapump) &&
		    detect_online_retrain(
			    datapump, &point, &retrain_started) != 0)
			return -1;
		if (retrain_started && !datapump->has_previous_receive_point)
			return 0;
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
	if (!datapump_set_link_rates(
		    &datapump->dp, datapump->bit_rate, datapump->bit_rate))
		return -1;
	datapump->training_state = V22_TRAINING_ONLINE;
	datapump->transmit_mode = V22_TRANSMIT_DATA;
	datapump->training_samples = 0;
	datapump->phase_samples = 0;
	datapump->scrambled_mark_bits = 0;
	datapump->v22bis_confirm_bits = 0;
	datapump->rate_signal_tx_symbols = 0;
	datapump->rate_signal_wait_samples = 0;
	datapump->missing_carrier_samples = 0;
	datapump->carrier_return_hold_samples = 0;
	datapump->retrain_peer_samples = 0;
	datapump->retrain_retry_samples = 0;
	discard_retrain_candidate(datapump);
	datapump->retrain_peer_signal_received = false;
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
		    V22_TRAINING_V22BIS_RATE_SIGNAL) {
			datapump->training_state =
				V22_TRAINING_WAIT_SCRAMBLED_MARK;
			datapump->rate_signal_wait_samples = 0;
			datapump->rate_signal_late_candidate = false;
			datapump->rate_signal_reply_sent = false;
			v22bis_rate_signal_detector_reset(
				&datapump->rate_signal_detector);
		}
	}
	if (datapump_is_retraining(datapump)) {
		if (datapump->retrain_peer_signal_received)
			datapump_add_samples_bounded(
				&datapump->retrain_peer_samples, 1,
				V22_RETRAIN_RETRY_SAMPLES);
		datapump_add_samples_bounded(
			&datapump->retrain_retry_samples, 1,
			V22_RETRAIN_RETRY_SAMPLES);
		if (datapump->training_state == V22_TRAINING_RETRAIN_MARK &&
		    datapump_uses_v22bis(datapump) &&
		    datapump->retrain_peer_samples >=
			    V22BIS_TRANSMIT_START_SAMPLES &&
		    datapump->v22bis_receiver_initialized) {
			if (start_v22bis_transmit(datapump) != 0)
				return -1;
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
		       datapump->rate_signal_detector.alternating_symbols <
			       V22_RETRAIN_MIN_SYMBOLS ?
			       restart_retrain(datapump) : 0;
	}
	if (datapump_uses_v22bis(datapump) && datapump->originator &&
	    datapump->training_state == V22_TRAINING_WAIT_SCRAMBLED_MARK)
		datapump_add_samples_bounded(
			&datapump->rate_signal_wait_samples, 1,
			V22_RETRAIN_RETRY_SAMPLES);
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
		if (datapump->phase_samples < V22_CONNECT_HOLD_SAMPLES)
			datapump->phase_samples++;
		if (datapump_uses_v22bis(datapump) &&
		    datapump->phase_samples >=
			    V22BIS_TRANSMIT_START_SAMPLES) {
			if (start_v22bis_transmit(datapump) != 0)
				return -1;
		} else if (!datapump_uses_v22bis(datapump) &&
			   datapump->phase_samples >=
				   V22_CONNECT_HOLD_SAMPLES &&
			   datapump->scrambled_mark_bits >=
				   scrambled_confirmation_target(datapump)) {
			/* The peer sends scrambled mark throughout this interval.  A
			 * fixed timer must not promote an ended automode probe. */
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
	bool line_power_present)
{
	return measurement->timing_locked && line_power_present;
}

static int process_v22(
	struct dp *dp, void *receive_buffer, void *transmit_buffer, int count)
{
	struct v22_datapump *datapump = v22_from_dp(dp);
	const int16_t *receive = receive_buffer;
	int16_t *transmit = transmit_buffer;
	struct qam_measurement measurement;
	uint32_t carrier_loss_limit;
	bool discard_receive = false;
	bool line_power_present;
	bool linked = false;
	bool return_hold_was_active;

	if (!datapump_linear_fragment_is_ready(datapump, receive, transmit, count))
		return DPSTAT_ERROR;
	line_power_present = datapump_qam_line_power_present(receive, count);
	/* The generic QAM floor is deliberately permissive.  Do not let codec
	 * idle residue establish V.22 timing or protocol lineage before carrier. */
	if (!line_power_present && !datapump_is_retraining(datapump) &&
	    datapump->training_state != V22_TRAINING_ONLINE) {
		if (reset_v22_receive_path(datapump) != 0) {
			memset(transmit, 0, (size_t)count * sizeof(*transmit));
			return DPSTAT_ERROR;
		}
		datapump->answer_mark_symbols = 0;
		datapump->scrambled_mark_bits = 0;
		datapump->v22bis_confirm_bits = 0;
		discard_retrain_candidate(datapump);
	}
	if (datapump->training_state != V22_TRAINING_ONLINE) {
		if ((uint64_t)(unsigned int)count >
		    datapump->training_timeout_samples - datapump->training_samples)
			return DPSTAT_ERROR;
		datapump->training_samples += (unsigned int)count;
	}
	if (datapump->training_state == V22_TRAINING_ONLINE &&
	    datapump_uses_v22bis(datapump) && line_power_present &&
	    datapump->missing_carrier_samples != 0 &&
	    datapump->missing_carrier_samples <
		    V22BIS_CARRIER_RETRAIN_SAMPLES) {
		datapump->missing_carrier_samples = 0;
		datapump->carrier_return_hold_samples =
			V22_CARRIER_RETURN_HOLD_SAMPLES;
	}
	if (datapump->training_state == V22_TRAINING_ONLINE &&
	    (datapump->missing_carrier_samples != 0 ||
	     !line_power_present)) {
		discard_receive = true;
		if (datapump->missing_carrier_samples == 0) {
			datapump->carrier_return_hold_samples = 0;
			if (flush_retrain_candidate(datapump) != 0)
				return DPSTAT_ERROR;
			discard_retrain_candidate(datapump);
			if (!datapump->dp.modem->cfg.ec)
				modem_async_retrain(datapump->dp.modem);
		}
	}
	return_hold_was_active =
		datapump->carrier_return_hold_samples != 0;
	for (int index = 0; index < count; ++index) {
		bool receive_active = line_power_present;
		if (datapump->originator && datapump->training_state ==
		    V22_TRAINING_ORIGINATE_WAIT_ANSWER_MARK) {
			struct single_tone_event event;
			if (single_tone_detector_sample(&datapump->answer_mark,
				    receive[index], &event) != SINGLE_TONE_OK)
				return DPSTAT_ERROR;
			receive_active = receive_active && datapump->answer_mark.tone_present;
			if (event.kind == SINGLE_TONE_ENDED) {
				datapump->answer_mark_symbols = 0;
				if (reset_v22_receive_path(datapump) != 0)
					return DPSTAT_ERROR;
			}
		}
		if (generate_sample(datapump, &transmit[index]) != 0 ||
		    (receive_active && receive_sample(
			    datapump, receive[index], discard_receive,
			    &linked) != 0) ||
		    advance_training_timer(datapump, &linked) != 0) {
			memset(transmit, 0, (size_t)count * sizeof(*transmit));
			return DPSTAT_ERROR;
		}
		if (datapump->training_state == V22_TRAINING_ONLINE &&
		    datapump->carrier_return_hold_samples != 0)
			datapump->carrier_return_hold_samples--;
	}
	if (linked)
		return DPSTAT_CONNECT;
	if (datapump->training_state != V22_TRAINING_ONLINE)
		return DPSTAT_OK;
	measurement = qam_demodulator_measure(
		&datapump->demodulator);
	carrier_loss_limit =
		datapump_carrier_loss_ticks(datapump->dp.modem);
	if (datapump_uses_v22bis(datapump) && line_power_present &&
	    (datapump->missing_carrier_samples >=
		     V22BIS_CARRIER_RETRAIN_SAMPLES ||
	     (return_hold_was_active &&
	      !carrier_is_present(&measurement, true)))) {
		if (begin_retrain(
			    datapump, receive_phase_quadrant(datapump), false) != 0)
			return DPSTAT_ERROR;
	} else if (carrier_is_present(&measurement, line_power_present)) {
		if (!datapump_uses_v22bis(datapump) &&
		    datapump->missing_carrier_samples != 0)
			datapump->carrier_return_hold_samples =
				V22_CARRIER_RETURN_HOLD_SAMPLES;
		datapump->missing_carrier_samples = 0;
	} else {
		datapump_add_samples_bounded(
			&datapump->missing_carrier_samples,
			(uint32_t)(unsigned int)count,
			carrier_loss_limit);
	}
	return datapump->missing_carrier_samples >= carrier_loss_limit ?
		       DPSTAT_ERROR : DPSTAT_OK;
}

/* Keep a validated S1 or scrambled-mark exchange through its own training
 * interval; an automatic answer-tone probe may expire only before that. */
bool v22_datapump_has_peer(struct dp *dp)
{
	struct v22_datapump *datapump = v22_from_dp(dp);

	return datapump != NULL &&
		(datapump->rate_signal_detector.present ||
		 datapump->training_state == V22_TRAINING_CONNECT_HOLD ||
		 datapump->training_state == V22_TRAINING_V22BIS_CONFIRM ||
		 datapump->training_state == V22_TRAINING_ONLINE);
}
