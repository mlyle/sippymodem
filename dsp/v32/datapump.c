// SPDX-License-Identifier: BSD-3-Clause

#include "datapump.h"

#include "modem.h"
#include "modem_param.h"
#include "sippymodem_qam.h"
#include "sippymodem_qam_gain.h"
#include "uncoded_receiver.h"
#include "conditioning.h"
#include "conditioning_receiver.h"
#include "constellation.h"
#include "framing.h"
#include "line_startup.h"
#include "startup.h"
#include "trellis.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* The caller performs line turnaround and the half-duplex receiver training
 * around the rate exchange. Two-wire echo cancellation remains follow-up
 * work. */
enum {
	V32_SYMBOL_RATE = 2400,
	V32_CARRIER_HZ = 1800,
	V32_PULSE_SPAN_SYMBOLS = 6,
	V32_TRANSMIT_AMPLITUDE = 12000,
	V32_SIGNAL_A = 0,
	V32_SIGNAL_B = 1,
	V32_CARRIER_LOSS_SAMPLES = MODEM_RATE / 5,
	V32_MIN_LINE_POWER = 1000000,
	V32_DIRECT_QAM_QUALIFY_SAMPLES =
		SIPPYMODEM_SINGLE_TONE_WINDOW_SAMPLES +
		SIPPYMODEM_SINGLE_TONE_ACQUIRE_SAMPLES,
};

static const struct sippymodem_qam_channel v32_channel = {
	.sample_rate = MODEM_RATE,
	.symbol_rate_numerator = V32_SYMBOL_RATE,
	.symbol_rate_denominator = 1,
	.carrier_frequency_numerator = V32_CARRIER_HZ,
	.carrier_frequency_denominator = 1,
	.rolloff = 0.25,
	.pulse_span_symbols = V32_PULSE_SPAN_SYMBOLS,
};

static const double V32_TRELLIS_CHANNEL_ADAPTATION = 1.0 / 32.0;

enum v32_line_entry {
	V32_LINE_ENTRY_CALLER_STARTUP,
	V32_LINE_ENTRY_CALLER_RECEIVE_CONDITIONING,
	V32_LINE_ENTRY_CALLER_RECEIVE_RATE_OFFER,
	V32_LINE_ENTRY_CALLER_TRANSMIT_CONDITIONING,
	V32_LINE_ENTRY_CALLER_RATE_EXCHANGE,
	V32_LINE_ENTRY_CALLER_RECEIVE_SECOND_CONDITIONING,
	V32_LINE_ENTRY_DIRECT_QAM_COMPATIBILITY,
};

struct sippymodem_v32_datapump {
	struct dp dp;
	struct sippymodem_qam_modulator modulator;
	struct sippymodem_qam_demodulator demodulator;
	struct sippymodem_v32_symbol_encoder encoder;
	struct sippymodem_v32_symbol_decoder decoder;
	struct sippymodem_v32_trellis_encoder trellis_encoder;
	struct sippymodem_v32_trellis_encoder known_trellis_encoder;
	struct sippymodem_v32_scrambler known_trellis_scrambler;
	struct sippymodem_v32_trellis_decoder trellis_decoder;
	struct sippymodem_v32_9600_receiver receiver_9600;
	struct sippymodem_v32_conditioning_transmitter
		conditioning_transmitter;
	struct sippymodem_v32_conditioning_receiver conditioning_receiver;
	struct sippymodem_v32_line_startup line_startup;
	struct sippymodem_v32_startup startup;
	struct sippymodem_qam_point transmit_point;
	struct sippymodem_qam_point previous_receive_point;
	struct sippymodem_qam_gain conditioning_gain;
	struct sippymodem_qam_gain trellis_gain;
	uint64_t training_samples;
	uint64_t training_timeout_samples;
	uint32_t missing_carrier_samples;
	uint32_t turnaround_s_symbols_remaining;
	uint16_t direct_qam_qualifying_samples;
	uint8_t receive_trellis_link_symbols;
	enum v32_line_entry line_entry;
	bool has_previous_receive_point;
	bool trellis_encoder_initialized;
	bool trellis_receiver_initialized;
	bool receiver_9600_initialized;
	bool second_conditioning_complete;
	bool second_conditioning_carrier_detected;
	bool linked;
	bool caller;
};

static struct dp *create_v32(
	struct modem *modem, enum DP_ID id, int caller, int sample_rate,
	int max_fragment, struct dp_operations *operations);
static int delete_v32(struct dp *dp);
static int process_v32(
	struct dp *dp, void *receive, void *transmit, int count);
static int enter_link(struct sippymodem_v32_datapump *datapump);

static struct dp_operations v32_operations = {
	.name = "sippymodem V.32",
	.create = create_v32,
	.delete = delete_v32,
	.process = process_v32,
};

static enum sippymodem_v32_transmitter_role local_role(bool caller)
{
	return caller ? SIPPYMODEM_V32_CALLING_TRANSMITTER :
			SIPPYMODEM_V32_ANSWERING_TRANSMITTER;
}

static enum sippymodem_v32_transmitter_role remote_role(bool caller)
{
	return caller ? SIPPYMODEM_V32_ANSWERING_TRANSMITTER :
			SIPPYMODEM_V32_CALLING_TRANSMITTER;
}

static enum sippymodem_v32_endpoint_role endpoint_role(bool caller)
{
	return caller ? SIPPYMODEM_V32_CALLER : SIPPYMODEM_V32_ANSWERER;
}

static bool caller_awaits_second_conditioning(
	const struct sippymodem_v32_startup *startup)
{
	return !startup->end_received &&
	       startup->phase == SIPPYMODEM_V32_CALLER_SEND_RESPONSE;
}

static bool caller_turnaround_s_symbols(
	const struct sippymodem_v32_line_startup *startup, uint32_t *symbols)
{
	uint64_t samples =
		sippymodem_v32_line_startup_round_trip_samples(startup);
	uint64_t rounded;

	if (samples == 0 || symbols == NULL)
		return false;
	rounded = (samples * V32_SYMBOL_RATE + MODEM_RATE - 1u) / MODEM_RATE;
	if ((rounded & 1u) != 0)
		rounded++;
	if (rounded == 0 || rounded > UINT32_MAX)
		return false;
	*symbols = (uint32_t)rounded;
	return true;
}

static bool point_is_finite(const struct sippymodem_qam_point *point)
{
	return point != NULL && isfinite(point->in_phase) &&
	       isfinite(point->quadrature);
}

static bool line_entry_is_valid(
	const struct sippymodem_v32_datapump *datapump)
{
	uint32_t turnaround_s_symbols = 0;

	if (datapump->line_entry < V32_LINE_ENTRY_CALLER_STARTUP ||
	    datapump->line_entry > V32_LINE_ENTRY_DIRECT_QAM_COMPATIBILITY ||
	    !sippymodem_v32_line_startup_is_valid(&datapump->line_startup) ||
	    !sippymodem_v32_conditioning_transmitter_is_valid(
		    &datapump->conditioning_transmitter) ||
	    datapump->conditioning_transmitter.trn_symbols !=
		    SIPPYMODEM_V32_CONDITIONING_TRN_MIN_SYMBOLS ||
	    datapump->conditioning_transmitter.trn_encoder.scrambler
			    .transmitter_role != local_role(datapump->caller) ||
	    !sippymodem_v32_conditioning_receiver_is_valid(
		    &datapump->conditioning_receiver) ||
	    datapump->conditioning_receiver.source_role !=
		    remote_role(datapump->caller) ||
	    (datapump->second_conditioning_carrier_detected &&
	     datapump->line_entry !=
		     V32_LINE_ENTRY_CALLER_RECEIVE_SECOND_CONDITIONING))
		return false;
	if (!datapump->caller)
		return datapump->line_entry ==
			       V32_LINE_ENTRY_DIRECT_QAM_COMPATIBILITY &&
		       datapump->direct_qam_qualifying_samples == 0 &&
		       datapump->turnaround_s_symbols_remaining == 0 &&
		       datapump->conditioning_transmitter.emitted_symbols == 0 &&
		       !datapump->second_conditioning_complete &&
		       !sippymodem_v32_conditioning_receiver_is_complete(
			       &datapump->conditioning_receiver);
	if (sippymodem_v32_line_startup_is_complete(&datapump->line_startup) &&
	    (!caller_turnaround_s_symbols(
		     &datapump->line_startup, &turnaround_s_symbols) ||
	     datapump->turnaround_s_symbols_remaining >
		     turnaround_s_symbols))
		return false;
	switch (datapump->line_entry) {
	case V32_LINE_ENTRY_CALLER_STARTUP:
		return !sippymodem_v32_line_startup_is_complete(
			&datapump->line_startup) &&
		       !sippymodem_v32_conditioning_receiver_is_complete(
			       &datapump->conditioning_receiver) &&
		       datapump->conditioning_transmitter.emitted_symbols == 0 &&
		       !datapump->has_previous_receive_point &&
		       datapump->turnaround_s_symbols_remaining == 0 &&
		       !datapump->second_conditioning_complete &&
		       datapump->line_startup.state !=
			       SIPPYMODEM_V32_LINE_STARTUP_FAILED &&
		       datapump->direct_qam_qualifying_samples <=
			       V32_DIRECT_QAM_QUALIFY_SAMPLES &&
		       (datapump->line_startup.state ==
					SIPPYMODEM_V32_LINE_WAIT_ANSWER_TONE ||
			datapump->direct_qam_qualifying_samples == 0);
	case V32_LINE_ENTRY_CALLER_RECEIVE_CONDITIONING:
		return sippymodem_v32_line_startup_is_complete(
			       &datapump->line_startup) &&
		       !sippymodem_v32_conditioning_receiver_is_complete(
			       &datapump->conditioning_receiver) &&
		       datapump->conditioning_transmitter.emitted_symbols == 0 &&
		       !datapump->has_previous_receive_point &&
		       datapump->turnaround_s_symbols_remaining ==
			       turnaround_s_symbols &&
		       !datapump->second_conditioning_complete &&
		       datapump->direct_qam_qualifying_samples == 0;
	case V32_LINE_ENTRY_CALLER_RECEIVE_RATE_OFFER:
		return sippymodem_v32_line_startup_is_complete(
			       &datapump->line_startup) &&
		       sippymodem_v32_conditioning_receiver_is_complete(
			       &datapump->conditioning_receiver) &&
		       sippymodem_qam_gain_is_valid(
			       &datapump->conditioning_gain) &&
		       datapump->has_previous_receive_point &&
		       datapump->conditioning_transmitter.emitted_symbols == 0 &&
		       datapump->turnaround_s_symbols_remaining ==
			       turnaround_s_symbols &&
		       !datapump->second_conditioning_complete &&
		       datapump->startup.phase ==
			       SIPPYMODEM_V32_CALLER_WAIT_OFFER &&
		       datapump->direct_qam_qualifying_samples == 0;
	case V32_LINE_ENTRY_CALLER_TRANSMIT_CONDITIONING:
		return sippymodem_v32_line_startup_is_complete(
			       &datapump->line_startup) &&
		       sippymodem_v32_conditioning_receiver_is_complete(
			       &datapump->conditioning_receiver) &&
		       sippymodem_qam_gain_is_valid(
			       &datapump->conditioning_gain) &&
		       datapump->has_previous_receive_point &&
		       !sippymodem_v32_conditioning_transmitter_is_complete(
			       &datapump->conditioning_transmitter) &&
		       !datapump->second_conditioning_complete &&
		       datapump->startup.phase ==
			       SIPPYMODEM_V32_CALLER_SEND_RESPONSE &&
		       datapump->direct_qam_qualifying_samples == 0;
	case V32_LINE_ENTRY_CALLER_RATE_EXCHANGE:
		return sippymodem_v32_line_startup_is_complete(
			       &datapump->line_startup) &&
		       sippymodem_v32_conditioning_receiver_is_complete(
			       &datapump->conditioning_receiver) &&
		       sippymodem_qam_gain_is_valid(
			       &datapump->conditioning_gain) &&
		       datapump->has_previous_receive_point &&
		       sippymodem_v32_conditioning_transmitter_is_complete(
			       &datapump->conditioning_transmitter) &&
		       datapump->turnaround_s_symbols_remaining == 0 &&
		       datapump->startup.phase !=
			       SIPPYMODEM_V32_CALLER_WAIT_OFFER &&
		       datapump->direct_qam_qualifying_samples == 0;
	case V32_LINE_ENTRY_CALLER_RECEIVE_SECOND_CONDITIONING:
		return sippymodem_v32_line_startup_is_complete(
			       &datapump->line_startup) &&
		       !sippymodem_v32_conditioning_receiver_is_complete(
			       &datapump->conditioning_receiver) &&
		       sippymodem_qam_gain_is_valid(
			       &datapump->conditioning_gain) &&
		       !datapump->has_previous_receive_point &&
		       sippymodem_v32_conditioning_transmitter_is_complete(
			       &datapump->conditioning_transmitter) &&
		       datapump->turnaround_s_symbols_remaining == 0 &&
		       !datapump->second_conditioning_complete &&
		       caller_awaits_second_conditioning(&datapump->startup) &&
		       !datapump->startup.detector.aligned &&
		       datapump->direct_qam_qualifying_samples == 0;
	case V32_LINE_ENTRY_DIRECT_QAM_COMPATIBILITY:
		return datapump->line_startup.state ==
			       SIPPYMODEM_V32_LINE_WAIT_ANSWER_TONE &&
		       datapump->conditioning_transmitter.emitted_symbols == 0 &&
		       datapump->turnaround_s_symbols_remaining == 0 &&
		       !datapump->second_conditioning_complete &&
		       !sippymodem_v32_conditioning_receiver_is_complete(
			       &datapump->conditioning_receiver) &&
		       datapump->direct_qam_qualifying_samples ==
			       V32_DIRECT_QAM_QUALIFY_SAMPLES;
	}
	return false;
}

static uint8_t rate_capabilities(const struct modem *modem)
{
	uint8_t rates = 0;

	if (modem == NULL)
		return 0;
	if (modem->min_rate <= SIPPYMODEM_V32_4800_BIT_RATE &&
	    modem->max_rate >= SIPPYMODEM_V32_4800_BIT_RATE)
		rates |= SIPPYMODEM_V32_RATE_CAPABILITY_4800;
	if (modem->min_rate <= SIPPYMODEM_V32_7200_BIT_RATE &&
	    modem->max_rate >= SIPPYMODEM_V32_7200_BIT_RATE)
		rates |= SIPPYMODEM_V32_RATE_CAPABILITY_7200;
	if (modem->min_rate <= SIPPYMODEM_V32_9600_BIT_RATE &&
	    modem->max_rate >= SIPPYMODEM_V32_9600_BIT_RATE)
		rates |= SIPPYMODEM_V32_RATE_CAPABILITY_9600;
	if (modem->min_rate <= SIPPYMODEM_V32_12000_BIT_RATE &&
	    modem->max_rate >= SIPPYMODEM_V32_12000_BIT_RATE)
		rates |= SIPPYMODEM_V32_RATE_CAPABILITY_12000;
	if (modem->min_rate <= SIPPYMODEM_V32_14400_BIT_RATE &&
	    modem->max_rate >= SIPPYMODEM_V32_14400_BIT_RATE)
		rates |= SIPPYMODEM_V32_RATE_CAPABILITY_14400;
	return rates;
}

static uint8_t selected_trellis_data_bits(
	const struct sippymodem_v32_datapump *datapump)
{
	switch (datapump->startup.selected_rate) {
	case SIPPYMODEM_V32_RATE_CAPABILITY_7200:
		return SIPPYMODEM_V32_7200_DATA_BITS;
	case SIPPYMODEM_V32_RATE_CAPABILITY_12000:
		return SIPPYMODEM_V32_12000_DATA_BITS;
	case SIPPYMODEM_V32_RATE_CAPABILITY_14400:
		return SIPPYMODEM_V32_14400_DATA_BITS;
	default:
		return 0;
	}
}

static bool uses_trellis(const struct sippymodem_v32_datapump *datapump)
{
	return selected_trellis_data_bits(datapump) != 0;
}

static bool uses_9600(const struct sippymodem_v32_datapump *datapump)
{
	return datapump->startup.selected_rate ==
	       SIPPYMODEM_V32_RATE_CAPABILITY_9600;
}

static bool trellis_receiver_is_valid(
	const struct sippymodem_v32_datapump *datapump)
{
	uint8_t data_bits = selected_trellis_data_bits(datapump);

	return sippymodem_v32_trellis_decoder_is_valid(
		       &datapump->trellis_decoder) &&
	       datapump->trellis_decoder.data_bits_per_symbol == data_bits &&
	       sippymodem_v32_trellis_encoder_is_valid(
		       &datapump->known_trellis_encoder) &&
	       datapump->known_trellis_encoder.data_bits_per_symbol == data_bits &&
	       sippymodem_v32_scrambler_is_valid(
		       &datapump->known_trellis_scrambler) &&
	       datapump->known_trellis_scrambler.transmitter_role ==
		       remote_role(datapump->caller) &&
	       sippymodem_qam_gain_is_valid(&datapump->trellis_gain) &&
	       datapump->receive_trellis_link_symbols <=
		       SIPPYMODEM_V32_TRELLIS_TRACEBACK_SYMBOLS;
}

static struct sippymodem_v32_datapump *v32_from_dp(struct dp *dp)
{
	struct sippymodem_v32_datapump *datapump;
	uint8_t trellis_data_bits;

	if (dp == NULL || dp->id != DP_V32 || dp->op != &v32_operations ||
	    dp->modem == NULL || dp->dp_data == NULL)
		return NULL;
	datapump = dp->dp_data;
	trellis_data_bits = selected_trellis_data_bits(datapump);
	if (&datapump->dp != dp ||
	    datapump->training_timeout_samples == 0 ||
	    datapump->training_samples > datapump->training_timeout_samples ||
	    datapump->missing_carrier_samples > V32_CARRIER_LOSS_SAMPLES ||
	    !line_entry_is_valid(datapump) ||
	    !sippymodem_v32_startup_is_valid(&datapump->startup) ||
	    datapump->startup.role != endpoint_role(datapump->caller) ||
	    datapump->startup.supported_rates != rate_capabilities(dp->modem) ||
	    datapump->linked !=
		    sippymodem_v32_startup_is_complete(&datapump->startup) ||
	    datapump->trellis_receiver_initialized !=
		    (trellis_data_bits != 0 && datapump->startup.end_received) ||
	    (datapump->trellis_receiver_initialized &&
	     !trellis_receiver_is_valid(datapump)) ||
	    (datapump->trellis_encoder_initialized &&
	     (trellis_data_bits == 0 ||
	      !sippymodem_v32_trellis_encoder_is_valid(
		      &datapump->trellis_encoder) ||
	      datapump->trellis_encoder.data_bits_per_symbol !=
		      trellis_data_bits)) ||
	    datapump->receiver_9600_initialized !=
		    (uses_9600(datapump) && datapump->startup.end_received) ||
	    (datapump->receiver_9600_initialized &&
	     !sippymodem_v32_9600_receiver_is_valid(
		     &datapump->receiver_9600)) ||
	    datapump->encoder.phase_quadrant >=
		    SIPPYMODEM_QPSK_QUADRANT_COUNT ||
	    datapump->decoder.phase_quadrant >=
		    SIPPYMODEM_QPSK_QUADRANT_COUNT ||
	    !sippymodem_v32_scrambler_is_valid(&datapump->encoder.scrambler) ||
	    !sippymodem_v32_scrambler_is_valid(
		    &datapump->decoder.descrambler) ||
	    datapump->encoder.scrambler.transmitter_role !=
		    local_role(datapump->caller) ||
	    datapump->decoder.descrambler.transmitter_role !=
		    remote_role(datapump->caller) ||
	    !point_is_finite(&datapump->transmit_point) ||
	    (datapump->has_previous_receive_point &&
	     !point_is_finite(&datapump->previous_receive_point)) ||
	    (datapump->linked && dp->status != DPSTAT_CONNECT) ||
	    (datapump->linked && trellis_data_bits != 0 &&
	     !datapump->trellis_encoder_initialized) ||
	    (!datapump->linked && dp->status == DPSTAT_CONNECT))
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

static struct dp *create_v32(
	struct modem *modem, enum DP_ID id, int caller, int sample_rate,
	int max_fragment, struct dp_operations *operations)
{
	struct sippymodem_v32_datapump *datapump;
	bool is_caller = caller != 0;
	uint8_t rates = rate_capabilities(modem);

	if (modem == NULL || id != DP_V32 || (caller != 0 && caller != 1) ||
	    sample_rate != MODEM_RATE || max_fragment != MODEM_FRAG ||
	    operations != &v32_operations || operations->use_count == INT_MAX ||
	    rates == 0)
		return NULL;
	datapump = calloc(1, sizeof(*datapump));
	if (datapump == NULL)
		return NULL;
	if (sippymodem_qam_modulator_init(
		    &datapump->modulator, &v32_channel,
		    V32_TRANSMIT_AMPLITUDE) != SIPPYMODEM_QAM_OK ||
	    sippymodem_qam_demodulator_init(
		    &datapump->demodulator, &v32_channel) != SIPPYMODEM_QAM_OK ||
	    sippymodem_v32_symbol_encoder_init(
		    &datapump->encoder, local_role(is_caller), 0) !=
		    SIPPYMODEM_V32_FRAMING_OK ||
	    sippymodem_v32_symbol_decoder_init(
		    &datapump->decoder, remote_role(is_caller), 0) !=
		    SIPPYMODEM_V32_FRAMING_OK ||
	    sippymodem_v32_startup_init(
		    &datapump->startup, endpoint_role(is_caller),
		    rates) !=
		    SIPPYMODEM_V32_STARTUP_OK ||
	    sippymodem_v32_line_startup_init(
		    &datapump->line_startup, V32_TRANSMIT_AMPLITUDE) !=
		    SIPPYMODEM_V32_LINE_STARTUP_OK ||
	    sippymodem_v32_conditioning_transmitter_init(
		    &datapump->conditioning_transmitter, local_role(is_caller),
		    SIPPYMODEM_V32_CONDITIONING_TRN_MIN_SYMBOLS) !=
		    SIPPYMODEM_V32_CONDITIONING_OK ||
	    sippymodem_v32_conditioning_receiver_init(
		    &datapump->conditioning_receiver, remote_role(is_caller)) !=
		    SIPPYMODEM_V32_CONDITIONING_RECEIVER_OK ||
	    sippymodem_v32_4800_constellation_point(
		    0, &datapump->transmit_point) !=
		    SIPPYMODEM_V32_CONSTELLATION_OK) {
		free(datapump);
		return NULL;
	}
	datapump->caller = is_caller;
	datapump->line_entry = is_caller ?
				 V32_LINE_ENTRY_CALLER_STARTUP :
				 V32_LINE_ENTRY_DIRECT_QAM_COMPATIBILITY;
	datapump->training_timeout_samples = training_timeout(modem);
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

static int delete_v32(struct dp *dp)
{
	struct sippymodem_v32_datapump *datapump = v32_from_dp(dp);

	if (datapump == NULL || v32_operations.use_count <= 0)
		return -1;
	v32_operations.use_count--;
	datapump->dp.dp_data = NULL;
	free(datapump);
	return 0;
}

static enum sippymodem_v32_framing_result encode_4800_transmit_quadrant(
	struct sippymodem_v32_symbol_encoder *encoder,
	enum sippymodem_v32_startup_output output, uint8_t input,
	uint8_t *quadrant)
{
	if (output == SIPPYMODEM_V32_STARTUP_TRN_DIBIT)
		return sippymodem_v32_trn_symbol_encode(
			encoder, input, quadrant);
	if (output == SIPPYMODEM_V32_STARTUP_DIFFERENTIAL_DIBIT ||
	    output == SIPPYMODEM_V32_STARTUP_SELECTED_RATE_MARK ||
	    output == SIPPYMODEM_V32_STARTUP_DATA)
		return sippymodem_v32_4800_symbol_encode(
			encoder, input, quadrant);
	return SIPPYMODEM_V32_FRAMING_INVALID_ARGUMENT;
}

static uint8_t pack_bits(const uint8_t *bits, size_t bit_count)
{
	uint8_t packed = 0;

	for (size_t index = 0; index < bit_count; ++index)
		packed = (uint8_t)(((unsigned int)packed << 1u) |
				   (unsigned int)(bits[index] & 1u));
	return packed;
}

static int encode_trellis_transmit_point(
	struct sippymodem_v32_datapump *datapump, uint8_t input)
{
	struct sippymodem_v32_trellis_encoder next_encoder;
	struct sippymodem_v32_scrambler next_scrambler =
		datapump->encoder.scrambler;
	struct sippymodem_qam_point next_point;
	uint8_t data_bits = selected_trellis_data_bits(datapump);
	uint8_t scrambled;
	uint8_t signal_symbol;

	if (data_bits == 0)
		return -1;
	if (datapump->trellis_encoder_initialized) {
		next_encoder = datapump->trellis_encoder;
	} else if (sippymodem_v32_trellis_encoder_init(
			   &next_encoder, data_bits, 0) !=
		   SIPPYMODEM_V32_TRELLIS_OK) {
		return -1;
	}
	if (sippymodem_v32_scramble_group(
		    &next_scrambler, input, data_bits,
		    &scrambled) != SIPPYMODEM_V32_FRAMING_OK ||
	    sippymodem_v32_trellis_encode(
		    &next_encoder, scrambled, &signal_symbol) !=
		    SIPPYMODEM_V32_TRELLIS_OK ||
	    sippymodem_v32_trellis_constellation_point(
		    data_bits, signal_symbol, &next_point) !=
		    SIPPYMODEM_V32_CONSTELLATION_OK)
		return -1;
	datapump->encoder.scrambler = next_scrambler;
	datapump->trellis_encoder = next_encoder;
	datapump->transmit_point = next_point;
	datapump->trellis_encoder_initialized = true;
	return 0;
}

static int next_transmit_point(
	struct sippymodem_v32_datapump *datapump, bool *linked)
{
	uint8_t input = 3;
	uint8_t symbol;
	uint8_t trellis_data_bits = selected_trellis_data_bits(datapump);
	size_t bit_count = SIPPYMODEM_V32_4800_BITS_PER_SYMBOL;
	enum sippymodem_v32_startup_output output;
	enum sippymodem_v32_framing_result framing_result;

	if (sippymodem_v32_startup_next_transmit(
		    &datapump->startup, &output, &input) !=
	    SIPPYMODEM_V32_STARTUP_OK)
		return -1;
	if (output == SIPPYMODEM_V32_STARTUP_DATA) {
		uint8_t bits[SIPPYMODEM_V32_TRELLIS_MAX_DATA_BITS] = {
			1, 1, 1, 1, 1, 1
		};

		if (!datapump->linked) {
			if (enter_link(datapump) != 0)
				return -1;
			*linked = true;
		}
		if (uses_9600(datapump))
			bit_count = SIPPYMODEM_V32_9600_BITS_PER_SYMBOL;
		else if (trellis_data_bits != 0)
			bit_count = trellis_data_bits;
		if (modem_get_bits(
			    datapump->dp.modem, 1, bits, (int)bit_count) !=
		    (int)bit_count)
			for (size_t index = 0; index < bit_count; ++index)
				bits[index] = 1;
		input = pack_bits(bits, bit_count);
	}
	if (output == SIPPYMODEM_V32_STARTUP_SELECTED_RATE_MARK &&
	    uses_9600(datapump))
		input = 15;
	if (output == SIPPYMODEM_V32_STARTUP_SELECTED_RATE_MARK &&
	    trellis_data_bits != 0)
		input = (uint8_t)((UINT8_C(1) << trellis_data_bits) - 1u);
	if (trellis_data_bits != 0 &&
	    (output == SIPPYMODEM_V32_STARTUP_SELECTED_RATE_MARK ||
	     output == SIPPYMODEM_V32_STARTUP_DATA)) {
		if (encode_trellis_transmit_point(datapump, input) != 0)
			return -1;
	} else if (uses_9600(datapump) &&
	    (output == SIPPYMODEM_V32_STARTUP_SELECTED_RATE_MARK ||
	     output == SIPPYMODEM_V32_STARTUP_DATA)) {
		framing_result = sippymodem_v32_9600_symbol_encode(
			&datapump->encoder, input, &symbol);
		if (framing_result != SIPPYMODEM_V32_FRAMING_OK ||
		    sippymodem_v32_constellation_point(
			    symbol, &datapump->transmit_point) !=
			    SIPPYMODEM_V32_CONSTELLATION_OK)
			return -1;
	} else {
		framing_result = encode_4800_transmit_quadrant(
			&datapump->encoder, output, input, &symbol);
		if (framing_result != SIPPYMODEM_V32_FRAMING_OK ||
		    sippymodem_v32_4800_constellation_point(
			    symbol, &datapump->transmit_point) !=
			    SIPPYMODEM_V32_CONSTELLATION_OK)
			return -1;
	}
	if (!datapump->linked &&
	    sippymodem_v32_startup_is_complete(&datapump->startup)) {
		if (enter_link(datapump) != 0)
			return -1;
		*linked = true;
	}
	return 0;
}

static int generate_caller_conditioning_sample(
	struct sippymodem_v32_datapump *datapump, int16_t *sample)
{
	bool can_accept = sippymodem_qam_modulator_can_accept_symbol(
		&datapump->modulator);
	bool accepted;

	if (can_accept) {
		uint8_t quadrant;

		if (datapump->turnaround_s_symbols_remaining != 0) {
			quadrant =
				(datapump->turnaround_s_symbols_remaining & 1u) != 0 ?
					V32_SIGNAL_B : V32_SIGNAL_A;
			datapump->turnaround_s_symbols_remaining--;
		} else if (sippymodem_v32_conditioning_transmitter_next(
				   &datapump->conditioning_transmitter, &quadrant) !=
			   SIPPYMODEM_V32_CONDITIONING_OK) {
			return -1;
		}
		if (sippymodem_v32_4800_constellation_point(
			    quadrant, &datapump->transmit_point) !=
		    SIPPYMODEM_V32_CONSTELLATION_OK)
			return -1;
	}
	if (sippymodem_qam_modulator_sample(
		    &datapump->modulator, &datapump->transmit_point, sample,
		    &accepted) != SIPPYMODEM_QAM_OK ||
	    accepted != can_accept)
		return -1;
	if (accepted &&
	    sippymodem_v32_conditioning_transmitter_is_complete(
		    &datapump->conditioning_transmitter)) {
		if (sippymodem_v32_conditioning_transmitter_framing_state(
			    &datapump->conditioning_transmitter,
			    &datapump->encoder) != SIPPYMODEM_V32_CONDITIONING_OK)
			return -1;
		datapump->line_entry = V32_LINE_ENTRY_CALLER_RATE_EXCHANGE;
	}
	return 0;
}

static int generate_sample(
	struct sippymodem_v32_datapump *datapump, int16_t *sample,
	bool *linked)
{
	bool can_accept;
	bool accepted;

	if (datapump->line_entry ==
	    V32_LINE_ENTRY_CALLER_TRANSMIT_CONDITIONING)
		return generate_caller_conditioning_sample(datapump, sample);

	if (datapump->caller &&
	    (datapump->line_entry ==
		     V32_LINE_ENTRY_CALLER_RECEIVE_CONDITIONING ||
	     datapump->line_entry ==
		     V32_LINE_ENTRY_CALLER_RECEIVE_RATE_OFFER)) {
		*sample = 0;
		return 0;
	}
	can_accept = sippymodem_qam_modulator_can_accept_symbol(
		&datapump->modulator);

	if ((can_accept && next_transmit_point(datapump, linked) != 0) ||
	    sippymodem_qam_modulator_sample(
		    &datapump->modulator, &datapump->transmit_point, sample,
		    &accepted) != SIPPYMODEM_QAM_OK ||
	    accepted != can_accept)
		return -1;
	return 0;
}

static int process_caller_line_startup(
	struct sippymodem_v32_datapump *datapump, int16_t receive_sample,
	int16_t *transmit_sample)
{
	struct sippymodem_qam_point ignored_point;
	struct sippymodem_qam_measurement measurement;
	enum sippymodem_v32_line_startup_result result;
	bool ignored_ready;
	bool complete;

	result = sippymodem_v32_line_startup_process(
		&datapump->line_startup, receive_sample, transmit_sample,
		&complete);
	if (result != SIPPYMODEM_V32_LINE_STARTUP_OK)
		return -1;
	if (complete) {
		if (!caller_turnaround_s_symbols(
			    &datapump->line_startup,
			    &datapump->turnaround_s_symbols_remaining))
			return -1;
		if (sippymodem_qam_demodulator_reset(&datapump->demodulator) !=
		    SIPPYMODEM_QAM_OK)
			return -1;
		datapump->has_previous_receive_point = false;
		datapump->line_entry =
			V32_LINE_ENTRY_CALLER_RECEIVE_CONDITIONING;
		return 0;
	}
	if (datapump->line_startup.state !=
	    SIPPYMODEM_V32_LINE_WAIT_ANSWER_TONE) {
		datapump->direct_qam_qualifying_samples = 0;
		return 0;
	}
	if (sippymodem_qam_demodulator_sample(
		    &datapump->demodulator, receive_sample, &ignored_point,
		    &ignored_ready) != SIPPYMODEM_QAM_OK)
		return -1;
	measurement = sippymodem_qam_demodulator_measure(
		&datapump->demodulator);
	if (measurement.timing_locked &&
	    measurement.line_power >= V32_MIN_LINE_POWER) {
		if (datapump->direct_qam_qualifying_samples <
		    V32_DIRECT_QAM_QUALIFY_SAMPLES)
			datapump->direct_qam_qualifying_samples++;
	} else {
		datapump->direct_qam_qualifying_samples = 0;
	}
	if (datapump->direct_qam_qualifying_samples ==
	    V32_DIRECT_QAM_QUALIFY_SAMPLES)
		datapump->line_entry = V32_LINE_ENTRY_DIRECT_QAM_COMPATIBILITY;
	return 0;
}

static unsigned int selected_bit_rate(
	const struct sippymodem_v32_datapump *datapump)
{
	switch (datapump->startup.selected_rate) {
	case SIPPYMODEM_V32_RATE_CAPABILITY_4800:
		return SIPPYMODEM_V32_4800_BIT_RATE;
	case SIPPYMODEM_V32_RATE_CAPABILITY_7200:
		return SIPPYMODEM_V32_7200_BIT_RATE;
	case SIPPYMODEM_V32_RATE_CAPABILITY_9600:
		return SIPPYMODEM_V32_9600_BIT_RATE;
	case SIPPYMODEM_V32_RATE_CAPABILITY_12000:
		return SIPPYMODEM_V32_12000_BIT_RATE;
	case SIPPYMODEM_V32_RATE_CAPABILITY_14400:
		return SIPPYMODEM_V32_14400_BIT_RATE;
	default:
		return 0;
	}
}

static int enter_link(struct sippymodem_v32_datapump *datapump)
{
	unsigned int bit_rate = selected_bit_rate(datapump);

	if (bit_rate == 0 ||
	    (uses_trellis(datapump) &&
	     (!datapump->trellis_receiver_initialized ||
	      !datapump->trellis_encoder_initialized)) ||
	    (uses_9600(datapump) && !datapump->receiver_9600_initialized))
		return -1;
	if (modem_set_param(
		    datapump->dp.modem, MDMPRM_TX_RATE, (int)bit_rate) != 0 ||
	    modem_set_param(
		    datapump->dp.modem, MDMPRM_RX_RATE, (int)bit_rate) != 0)
		return -1;
	datapump->linked = true;
	datapump->missing_carrier_samples = 0;
	datapump->dp.status = DPSTAT_CONNECT;
	return 0;
}

static void deliver_bits(
	struct sippymodem_v32_datapump *datapump, uint8_t value,
	size_t bit_count)
{
	uint8_t bits[SIPPYMODEM_V32_TRELLIS_MAX_DATA_BITS];

	for (size_t index = 0; index < bit_count; ++index)
		bits[index] = (uint8_t)((value >> (bit_count - index - 1u)) & 1u);
	if (datapump->dp.modem->put_bits != NULL)
		(void)modem_put_bits(
			datapump->dp.modem, 1, bits, (int)bit_count);
}

static int initialize_trellis_receiver(
	struct sippymodem_v32_datapump *datapump,
	const struct sippymodem_qam_point *reference_point)
{
	struct sippymodem_v32_trellis_encoder known_encoder;
	struct sippymodem_v32_trellis_decoder decoder;
	struct sippymodem_qam_gain gain;
	struct sippymodem_qam_point ideal;
	uint8_t data_bits = selected_trellis_data_bits(datapump);

	if (data_bits == 0 ||
	    sippymodem_v32_4800_constellation_point(
		    datapump->decoder.phase_quadrant, &ideal) !=
		    SIPPYMODEM_V32_CONSTELLATION_OK ||
	    sippymodem_qam_gain_init_from_reference(
		    &gain, reference_point, &ideal,
		    V32_TRELLIS_CHANNEL_ADAPTATION) != SIPPYMODEM_QAM_GAIN_OK ||
	    sippymodem_v32_trellis_encoder_init(
		    &known_encoder, data_bits, 0) !=
		    SIPPYMODEM_V32_TRELLIS_OK ||
	    sippymodem_v32_trellis_decoder_init(
		    &decoder, data_bits, 0) !=
		    SIPPYMODEM_V32_TRELLIS_OK)
		return -1;
	datapump->known_trellis_encoder = known_encoder;
	datapump->known_trellis_scrambler = datapump->decoder.descrambler;
	datapump->trellis_decoder = decoder;
	datapump->trellis_gain = gain;
	datapump->receive_trellis_link_symbols = 0;
	datapump->trellis_receiver_initialized = true;
	return 0;
}

static int track_known_trellis_point(
	struct sippymodem_v32_datapump *datapump,
	const struct sippymodem_qam_point *point, uint8_t input,
	bool *confirmed)
{
	struct sippymodem_v32_scrambler known_scrambler =
		datapump->known_trellis_scrambler;
	struct sippymodem_v32_scrambler output_descrambler =
		datapump->decoder.descrambler;
	struct sippymodem_v32_trellis_encoder known_encoder =
		datapump->known_trellis_encoder;
	struct sippymodem_v32_trellis_decoder decoder =
		datapump->trellis_decoder;
	struct sippymodem_qam_gain gain = datapump->trellis_gain;
	struct sippymodem_qam_point equalized;
	struct sippymodem_qam_point transmitted;
	uint8_t data_bits = selected_trellis_data_bits(datapump);
	uint8_t decoded = 0;
	uint8_t plain = 0;
	uint8_t scrambled;
	uint8_t signal_symbol;
	bool ready;

	if (!datapump->trellis_receiver_initialized || data_bits == 0 ||
	    confirmed == NULL ||
	    sippymodem_v32_scramble_group(
		    &known_scrambler, input, data_bits,
		    &scrambled) != SIPPYMODEM_V32_FRAMING_OK ||
	    sippymodem_v32_trellis_encode(
		    &known_encoder, scrambled, &signal_symbol) !=
		    SIPPYMODEM_V32_TRELLIS_OK ||
	    sippymodem_v32_trellis_constellation_point(
		    data_bits, signal_symbol, &transmitted) !=
		    SIPPYMODEM_V32_CONSTELLATION_OK ||
	    sippymodem_qam_gain_track(&gain, point, &transmitted) !=
		    SIPPYMODEM_QAM_GAIN_OK ||
	    sippymodem_qam_gain_equalize(&gain, point, &equalized) !=
		    SIPPYMODEM_QAM_GAIN_OK ||
	    sippymodem_v32_trellis_decode(
		    &decoder, &equalized, &decoded, &ready) !=
		    SIPPYMODEM_V32_TRELLIS_OK ||
	    (ready &&
	     sippymodem_v32_descramble_group(
		     &output_descrambler, decoded, data_bits, &plain) !=
		     SIPPYMODEM_V32_FRAMING_OK))
		return -1;
	datapump->decoder.descrambler = output_descrambler;
	datapump->known_trellis_encoder = known_encoder;
	datapump->known_trellis_scrambler = known_scrambler;
	datapump->trellis_decoder = decoder;
	datapump->trellis_gain = gain;
	*confirmed = ready && plain == input;
	return 0;
}

static int decode_trellis_point(
	struct sippymodem_v32_datapump *datapump,
	const struct sippymodem_qam_point *point, uint8_t *output, bool *ready)
{
	struct sippymodem_v32_scrambler descrambler =
		datapump->decoder.descrambler;
	struct sippymodem_v32_trellis_decoder decoder =
		datapump->trellis_decoder;
	struct sippymodem_qam_point equalized;
	uint8_t data_bits = selected_trellis_data_bits(datapump);
	uint8_t link_symbols = datapump->receive_trellis_link_symbols;
	uint8_t decoded = 0;
	uint8_t scrambled = 0;
	bool trellis_ready;
	bool payload_ready;

	if (!datapump->trellis_receiver_initialized || data_bits == 0 ||
	    output == NULL ||
	    ready == NULL ||
	    sippymodem_qam_gain_equalize(
		    &datapump->trellis_gain, point, &equalized) !=
		    SIPPYMODEM_QAM_GAIN_OK ||
	    sippymodem_v32_trellis_decode(
		    &decoder, &equalized, &scrambled, &trellis_ready) !=
		    SIPPYMODEM_V32_TRELLIS_OK)
		return -1;
	if (link_symbols < SIPPYMODEM_V32_TRELLIS_TRACEBACK_SYMBOLS)
		link_symbols++;
	payload_ready =
		trellis_ready &&
		link_symbols == SIPPYMODEM_V32_TRELLIS_TRACEBACK_SYMBOLS;
	if (trellis_ready &&
	    sippymodem_v32_descramble_group(
		    &descrambler, scrambled, data_bits,
		    &decoded) != SIPPYMODEM_V32_FRAMING_OK)
		return -1;
	datapump->decoder.descrambler = descrambler;
	datapump->trellis_decoder = decoder;
	datapump->receive_trellis_link_symbols = link_symbols;
	*ready = payload_ready;
	if (payload_ready)
		*output = decoded;
	return 0;
}

static int reset_second_conditioning_receiver(
	struct sippymodem_v32_datapump *datapump, bool carrier_detected)
{
	struct sippymodem_v32_conditioning_receiver receiver;
	struct sippymodem_qam_demodulator demodulator = datapump->demodulator;

	if (sippymodem_v32_conditioning_receiver_init(
		    &receiver, remote_role(datapump->caller)) !=
		    SIPPYMODEM_V32_CONDITIONING_RECEIVER_OK ||
	    sippymodem_qam_demodulator_reset(&demodulator) !=
		    SIPPYMODEM_QAM_OK)
		return -1;
	datapump->conditioning_receiver = receiver;
	datapump->demodulator = demodulator;
	datapump->has_previous_receive_point = false;
	datapump->second_conditioning_carrier_detected = carrier_detected;
	return 0;
}

static int observe_dibit(
	struct sippymodem_v32_datapump *datapump, uint8_t dibit,
	const struct sippymodem_qam_point *point, bool *linked)
{
	if (datapump->linked) {
		if (uses_9600(datapump) || uses_trellis(datapump))
			return -1;
		deliver_bits(
			datapump, dibit, SIPPYMODEM_V32_4800_BITS_PER_SYMBOL);
		return 0;
	}
	{
		bool connected;
		bool end_was_received = datapump->startup.end_received;
		enum sippymodem_v32_startup_phase previous_phase =
			datapump->startup.phase;

		if (sippymodem_v32_startup_observe(
			    &datapump->startup, dibit, &connected) !=
			    SIPPYMODEM_V32_STARTUP_OK)
			return -1;
		if (datapump->line_entry ==
			    V32_LINE_ENTRY_CALLER_RECEIVE_RATE_OFFER &&
		    previous_phase == SIPPYMODEM_V32_CALLER_WAIT_OFFER &&
		    datapump->startup.phase ==
			    SIPPYMODEM_V32_CALLER_SEND_RESPONSE)
			datapump->line_entry =
				V32_LINE_ENTRY_CALLER_TRANSMIT_CONDITIONING;
		if (datapump->line_entry == V32_LINE_ENTRY_CALLER_RATE_EXCHANGE &&
		    !datapump->second_conditioning_complete &&
		    caller_awaits_second_conditioning(&datapump->startup) &&
		    !datapump->startup.detector.aligned &&
		    datapump->startup.rate_gap_dibits ==
			    SIPPYMODEM_V32_STARTUP_RATE_GAP_DIBITS) {
			if (reset_second_conditioning_receiver(datapump, false) != 0)
				return -1;
			datapump->line_entry =
				V32_LINE_ENTRY_CALLER_RECEIVE_SECOND_CONDITIONING;
		}
		if (uses_trellis(datapump) && !end_was_received &&
		    datapump->startup.end_received) {
			if (initialize_trellis_receiver(datapump, point) != 0)
				return -1;
		} else if (uses_9600(datapump) && !end_was_received &&
			   datapump->startup.end_received) {
			if (sippymodem_v32_9600_receiver_init(
				    &datapump->receiver_9600, &datapump->decoder,
				    point) != SIPPYMODEM_V32_9600_RECEIVER_OK)
				return -1;
			datapump->receiver_9600_initialized = true;
		}
		if (!connected)
			return 0;
		if (enter_link(datapump) != 0)
			return -1;
		*linked = true;
	}
	return 0;
}

static int observe_trellis_point(
	struct sippymodem_v32_datapump *datapump,
	const struct sippymodem_qam_point *point, bool *linked)
{
	uint8_t data_bits = selected_trellis_data_bits(datapump);
	uint8_t mark = (uint8_t)((UINT8_C(1) << data_bits) - 1u);

	if (!datapump->trellis_receiver_initialized || data_bits == 0)
		return -1;
	if (datapump->linked) {
		uint8_t bits;
		bool ready;

		if (decode_trellis_point(datapump, point, &bits, &ready) != 0)
			return -1;
		if (ready)
			deliver_bits(datapump, bits, data_bits);
	} else {
		bool connected;
		bool confirmed;

		if (track_known_trellis_point(
			    datapump, point, mark, &confirmed) != 0 ||
		    sippymodem_v32_startup_observe(
			    &datapump->startup, confirmed ? 3 : 0,
			    &connected) != SIPPYMODEM_V32_STARTUP_OK)
			return -1;
		if (connected) {
			if (enter_link(datapump) != 0)
				return -1;
			*linked = true;
		}
	}
	return 0;
}

static int observe_9600_point(
	struct sippymodem_v32_datapump *datapump,
	const struct sippymodem_qam_point *point, bool *linked)
{
	if (!datapump->receiver_9600_initialized)
		return -1;
	if (datapump->linked) {
		uint8_t bits;
		bool ready;

		if (sippymodem_v32_9600_receiver_decode(
			    &datapump->receiver_9600, point, &bits, &ready) !=
		    SIPPYMODEM_V32_9600_RECEIVER_OK)
			return -1;
		if (ready)
			deliver_bits(
				datapump, bits,
				SIPPYMODEM_V32_9600_BITS_PER_SYMBOL);
	} else {
		bool connected;

		if (sippymodem_v32_9600_receiver_track_known(
			    &datapump->receiver_9600, point, 15) !=
			    SIPPYMODEM_V32_9600_RECEIVER_OK ||
		    sippymodem_v32_startup_observe(
			    &datapump->startup, 3,
			    &connected) != SIPPYMODEM_V32_STARTUP_OK)
			return -1;
		if (connected) {
			if (enter_link(datapump) != 0)
				return -1;
			*linked = true;
		}
	}
	return 0;
}

static int receive_sample(
	struct sippymodem_v32_datapump *datapump, int16_t sample,
	bool *linked)
{
	struct sippymodem_qam_point point;
	struct sippymodem_qam_measurement measurement;
	bool ready;

	if (sippymodem_qam_demodulator_sample(
		    &datapump->demodulator, sample, &point, &ready) !=
	    SIPPYMODEM_QAM_OK)
		return -1;
	if (datapump->line_entry ==
		    V32_LINE_ENTRY_CALLER_RECEIVE_SECOND_CONDITIONING &&
	    !datapump->second_conditioning_carrier_detected) {
		measurement = sippymodem_qam_demodulator_measure(
			&datapump->demodulator);
		if (measurement.line_power < V32_MIN_LINE_POWER)
			return 0;
		return reset_second_conditioning_receiver(datapump, true);
	}
	if (!ready)
		return 0;
	if (datapump->line_entry ==
		    V32_LINE_ENTRY_CALLER_RECEIVE_CONDITIONING ||
	    datapump->line_entry ==
		    V32_LINE_ENTRY_CALLER_RECEIVE_SECOND_CONDITIONING) {
		enum v32_line_entry next_entry =
			datapump->line_entry ==
				V32_LINE_ENTRY_CALLER_RECEIVE_CONDITIONING ?
				V32_LINE_ENTRY_CALLER_RECEIVE_RATE_OFFER :
				V32_LINE_ENTRY_CALLER_RATE_EXCHANGE;
		bool conditioning_ready;
		bool second_conditioning =
			datapump->line_entry ==
			V32_LINE_ENTRY_CALLER_RECEIVE_SECOND_CONDITIONING;

		if (sippymodem_v32_conditioning_receiver_observe(
			    &datapump->conditioning_receiver, &point,
			    &conditioning_ready) !=
			    SIPPYMODEM_V32_CONDITIONING_RECEIVER_OK)
			return -1;
		if (conditioning_ready) {
			if (sippymodem_v32_conditioning_receiver_framing_state(
				    &datapump->conditioning_receiver,
				    &datapump->decoder,
				    &datapump->conditioning_gain) !=
			    SIPPYMODEM_V32_CONDITIONING_RECEIVER_OK)
				return -1;
			datapump->previous_receive_point = point;
			datapump->has_previous_receive_point = true;
			datapump->second_conditioning_complete =
				second_conditioning;
			datapump->second_conditioning_carrier_detected = false;
			datapump->line_entry = next_entry;
		}
		return 0;
	}
	if (datapump->trellis_receiver_initialized) {
		if (observe_trellis_point(datapump, &point, linked) != 0)
			return -1;
		datapump->previous_receive_point = point;
		datapump->has_previous_receive_point = true;
		return 0;
	}
	if (datapump->receiver_9600_initialized) {
		if (observe_9600_point(datapump, &point, linked) != 0)
			return -1;
		datapump->previous_receive_point = point;
		datapump->has_previous_receive_point = true;
		return 0;
	}
	if (datapump->has_previous_receive_point) {
		uint8_t dibit;
		uint8_t quarter_turns;
		uint8_t quadrant;

		if (sippymodem_qam_nearest_quarter_turn(
			    &datapump->previous_receive_point, &point,
			    &quarter_turns) != SIPPYMODEM_QAM_OK)
			return -1;
		quadrant = (uint8_t)((datapump->decoder.phase_quadrant +
				      quarter_turns) &
				     3u);
		if (sippymodem_v32_4800_symbol_decode(
			    &datapump->decoder, quadrant, &dibit) !=
			    SIPPYMODEM_V32_FRAMING_OK ||
		    observe_dibit(datapump, dibit, &point, linked) != 0)
			return -1;
	}
	if (datapump->line_entry ==
	    V32_LINE_ENTRY_CALLER_RECEIVE_SECOND_CONDITIONING)
		return 0;
	datapump->previous_receive_point = point;
	datapump->has_previous_receive_point = true;
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
	       power / (unsigned int)count >= V32_MIN_LINE_POWER;
}

static void add_bounded(
	uint32_t *value, uint32_t increment, uint32_t limit)
{
	*value = *value >= limit || increment >= limit - *value ?
		       limit : *value + increment;
}

static int process_v32(
	struct dp *dp, void *receive_buffer, void *transmit_buffer, int count)
{
	struct sippymodem_v32_datapump *datapump = v32_from_dp(dp);
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
	if (!datapump->linked) {
		if ((uint64_t)(unsigned int)count >
		    datapump->training_timeout_samples -
			    datapump->training_samples)
			return DPSTAT_ERROR;
		datapump->training_samples += (unsigned int)count;
	}
	for (int index = 0; index < count; ++index) {
		if (datapump->line_entry == V32_LINE_ENTRY_CALLER_STARTUP) {
			if (process_caller_line_startup(
				    datapump, receive[index], &transmit[index]) != 0) {
				memset(transmit, 0,
				       (size_t)count * sizeof(*transmit));
				return DPSTAT_ERROR;
			}
			continue;
		}
		if (generate_sample(datapump, &transmit[index], &linked) != 0 ||
		    receive_sample(datapump, receive[index], &linked) != 0) {
			memset(transmit, 0, (size_t)count * sizeof(*transmit));
			return DPSTAT_ERROR;
		}
	}
	if (linked)
		return DPSTAT_CONNECT;
	if (!datapump->linked)
		return DPSTAT_OK;
	measurement = sippymodem_qam_demodulator_measure(
		&datapump->demodulator);
	if (carrier_is_present(&measurement, receive, count)) {
		datapump->missing_carrier_samples = 0;
	} else {
		add_bounded(
			&datapump->missing_carrier_samples,
			(uint32_t)(unsigned int)count,
			V32_CARRIER_LOSS_SAMPLES);
	}
	return datapump->missing_carrier_samples >=
			       V32_CARRIER_LOSS_SAMPLES ?
		       DPSTAT_ERROR :
		       DPSTAT_OK;
}

int sippymodem_v32_datapump_register(void)
{
	return modem_dp_register(DP_V32, &v32_operations);
}

int sippymodem_v32_datapump_unregister(void)
{
	return modem_dp_deregister(DP_V32, &v32_operations);
}

bool sippymodem_v32_datapump_is_busy(void)
{
	return v32_operations.use_count != 0;
}
