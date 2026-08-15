// SPDX-License-Identifier: BSD-3-Clause

#include "datapump.h"

#include "datapump_timing.h"
#include "modem.h"
#include "modem_param.h"
#include "qam.h"
#include "qam_gain.h"
#include "uncoded_receiver.h"
#include "conditioning.h"
#include "conditioning_receiver.h"
#include "constellation.h"
#include "framing.h"
#include "line_startup.h"
#include "startup.h"
#include "trellis.h"
#include "trellis_acquirer.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* The endpoints perform line turnaround and half-duplex receiver training
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
	/* Direct-QAM entry has no analog handoff, so wait through the complete
	 * answerer retraining interval after its two-word R2 acquisition. */
	V32_DIRECT_RETRAIN_DIBITS =
		V32_STARTUP_TRAINING_DIBITS +
		2 * V32_RATE_SIGNAL_DIBITS,
	V32_DIRECT_QAM_QUALIFY_SAMPLES =
		SINGLE_TONE_WINDOW_SAMPLES +
		SINGLE_TONE_ACQUIRE_SAMPLES,
	V32_CONDITIONING_SILENCE_SAMPLES = 24,
	V32_CONDITIONING_SIGNAL_THRESHOLD = 256,
};

static const struct qam_channel v32_channel = {
	.sample_rate = MODEM_RATE,
	.symbol_rate_numerator = V32_SYMBOL_RATE,
	.symbol_rate_denominator = 1,
	.carrier_frequency_numerator = V32_CARRIER_HZ,
	.carrier_frequency_denominator = 1,
	.rolloff = 0.25,
	.pulse_span_symbols = V32_PULSE_SPAN_SYMBOLS,
};

static const double v32_min_conditioning_point_power = 1e-12;

enum v32_line_entry {
	V32_LINE_ENTRY_CALLER_STARTUP,
	V32_LINE_ENTRY_CALLER_RECEIVE_CONDITIONING,
	V32_LINE_ENTRY_CALLER_RECEIVE_RATE_OFFER,
	V32_LINE_ENTRY_CALLER_TRANSMIT_CONDITIONING,
	V32_LINE_ENTRY_CALLER_RATE_EXCHANGE,
	V32_LINE_ENTRY_CALLER_RECEIVE_SECOND_CONDITIONING,
	V32_LINE_ENTRY_ANSWERER_STARTUP,
	V32_LINE_ENTRY_ANSWERER_PROTOCOL,
	V32_LINE_ENTRY_DIRECT_QAM_COMPATIBILITY,
};

struct v32_datapump {
	struct dp dp;
	struct qam_modulator modulator;
	struct qam_demodulator demodulator;
	struct v32_symbol_encoder encoder;
	struct v32_symbol_decoder decoder;
	struct v32_trellis_encoder trellis_encoder;
	struct v32_trellis_acquirer trellis_acquirer;
	struct v32_9600_receiver receiver_9600;
	struct v32_conditioning_transmitter
		conditioning_transmitter;
	struct v32_conditioning_receiver conditioning_receiver;
	struct v32_line_startup line_startup;
	struct v32_answerer_line_startup answerer_line_startup;
	struct v32_startup startup;
	struct qam_point transmit_point;
	struct qam_point previous_receive_point;
	struct qam_gain conditioning_gain;
	uint64_t training_samples;
	uint64_t training_timeout_samples;
	uint32_t missing_carrier_samples;
	uint32_t turnaround_s_symbols_remaining;
	uint16_t direct_retrain_dibits;
	uint16_t direct_qam_qualifying_samples;
	uint8_t conditioning_silence_samples;
	enum v32_line_entry line_entry;
	bool has_previous_receive_point;
	bool trellis_encoder_initialized;
	bool trellis_receiver_initialized;
	bool receiver_9600_initialized;
	bool second_conditioning_complete;
	bool conditioning_carrier_detected;
	bool linked;
	bool caller;
};

static struct dp *create_v32(
	struct modem *modem, enum DP_ID id, int caller, int sample_rate,
	int max_fragment, struct dp_operations *operations);
static int delete_v32(struct dp *dp);
static int process_v32(
	struct dp *dp, void *receive, void *transmit, int count);
static int enter_link(struct v32_datapump *datapump);

static struct dp_operations v32_operations = {
	.name = "V.32",
	.create = create_v32,
	.delete = delete_v32,
	.process = process_v32,
};

static enum v32_transmitter_role local_role(bool caller)
{
	return caller ? V32_CALLING_TRANSMITTER :
			V32_ANSWERING_TRANSMITTER;
}

static enum v32_transmitter_role remote_role(bool caller)
{
	return caller ? V32_ANSWERING_TRANSMITTER :
			V32_CALLING_TRANSMITTER;
}

static enum v32_endpoint_role endpoint_role(bool caller)
{
	return caller ? V32_CALLER : V32_ANSWERER;
}

static bool receives_conditioning(enum v32_line_entry entry)
{
	return entry == V32_LINE_ENTRY_CALLER_RECEIVE_CONDITIONING ||
	       entry == V32_LINE_ENTRY_CALLER_RECEIVE_SECOND_CONDITIONING;
}

static bool caller_awaits_second_conditioning(
	const struct v32_startup *startup)
{
	return !startup->end_received &&
	       startup->phase == V32_CALLER_SEND_RESPONSE;
}

static bool caller_turnaround_s_symbols(
	const struct v32_line_startup *startup, uint32_t *symbols)
{
	uint64_t samples =
		v32_line_startup_round_trip_samples(startup);
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

static bool answerer_training_is_valid(
	const struct v32_datapump *datapump)
{
	uint16_t fixed_symbols = V32_CONDITIONING_S_SYMBOLS +
				 V32_CONDITIONING_S_BAR_SYMBOLS;
	uint16_t emitted = datapump->conditioning_transmitter.emitted_symbols;
	uint16_t training = emitted > fixed_symbols ?
				    (uint16_t)(emitted - fixed_symbols) : 0;
	bool transmitting_training =
		datapump->startup.phase == V32_ANSWERER_TRAIN ||
		datapump->startup.phase == V32_ANSWERER_RETRAIN;

	if (transmitting_training)
		return !v32_conditioning_transmitter_is_complete(
			       &datapump->conditioning_transmitter) &&
		       datapump->startup.phase_dibits == training;
	return v32_conditioning_transmitter_is_complete(
		&datapump->conditioning_transmitter);
}

static bool answerer_line_entry_is_valid(
	const struct v32_datapump *datapump)
{
	bool received_conditioning =
		v32_conditioning_receiver_is_complete(
			&datapump->conditioning_receiver);
	bool needs_received_conditioning =
		datapump->startup.phase >= V32_ANSWERER_RETRAIN;

	if (datapump->line_startup.state != V32_LINE_WAIT_ANSWER_TONE ||
	    datapump->direct_qam_qualifying_samples != 0 ||
	    datapump->direct_retrain_dibits != 0 ||
	    datapump->turnaround_s_symbols_remaining != 0 ||
	    datapump->second_conditioning_complete ||
	    datapump->conditioning_carrier_detected ||
	    datapump->conditioning_silence_samples != 0 ||
	    (received_conditioning &&
	     (!datapump->has_previous_receive_point ||
	      !qam_gain_is_valid(&datapump->conditioning_gain))) ||
	    (needs_received_conditioning && !received_conditioning))
		return false;
	if (datapump->line_entry == V32_LINE_ENTRY_ANSWERER_STARTUP)
		return !v32_answerer_line_startup_is_complete(
			       &datapump->answerer_line_startup) &&
		       datapump->answerer_line_startup.state !=
			       V32_ANSWERER_LINE_FAILED &&
		       datapump->startup.phase == V32_ANSWERER_TRAIN &&
		       datapump->startup.phase_dibits == 0 &&
		       datapump->conditioning_transmitter.emitted_symbols == 0 &&
		       !received_conditioning &&
		       !datapump->has_previous_receive_point;
	if (datapump->line_entry != V32_LINE_ENTRY_ANSWERER_PROTOCOL ||
	    !v32_answerer_line_startup_is_complete(
		    &datapump->answerer_line_startup) ||
	    !answerer_training_is_valid(datapump))
		return false;
	return received_conditioning == datapump->has_previous_receive_point;
}

static bool line_entry_is_valid(
	const struct v32_datapump *datapump)
{
	uint32_t turnaround_s_symbols = 0;

	if (datapump->line_entry < V32_LINE_ENTRY_CALLER_STARTUP ||
	    datapump->line_entry > V32_LINE_ENTRY_DIRECT_QAM_COMPATIBILITY ||
	    !v32_line_startup_is_valid(&datapump->line_startup) ||
	    !v32_answerer_line_startup_is_valid(
		    &datapump->answerer_line_startup) ||
	    !v32_conditioning_transmitter_is_valid(
		    &datapump->conditioning_transmitter) ||
	    datapump->conditioning_transmitter.trn_symbols !=
		    V32_CONDITIONING_TRN_MIN_SYMBOLS ||
	    datapump->conditioning_transmitter.trn_encoder.scrambler
			    .transmitter_role != local_role(datapump->caller) ||
	    !v32_conditioning_receiver_is_valid(
		    &datapump->conditioning_receiver) ||
	    datapump->conditioning_receiver.source_role !=
		    remote_role(datapump->caller) ||
	    datapump->conditioning_silence_samples >
		    V32_CONDITIONING_SILENCE_SAMPLES ||
	    (datapump->conditioning_carrier_detected &&
	     !receives_conditioning(datapump->line_entry)) ||
	    (datapump->conditioning_silence_samples != 0 &&
	     !receives_conditioning(datapump->line_entry)) ||
	    datapump->direct_retrain_dibits > V32_DIRECT_RETRAIN_DIBITS ||
	    (datapump->direct_retrain_dibits != 0 &&
	     (datapump->line_entry != V32_LINE_ENTRY_DIRECT_QAM_COMPATIBILITY ||
	      !datapump->caller ||
	      datapump->startup.phase !=
		      V32_CALLER_SEND_RESPONSE ||
	      datapump->startup.selected_rate != 0 ||
	      datapump->startup.selection_expected)))
		return false;
	if (!datapump->caller)
		return answerer_line_entry_is_valid(datapump);
	if (v32_line_startup_is_complete(&datapump->line_startup) &&
	    (!caller_turnaround_s_symbols(
		     &datapump->line_startup, &turnaround_s_symbols) ||
	     datapump->turnaround_s_symbols_remaining >
		     turnaround_s_symbols))
		return false;
	switch (datapump->line_entry) {
	case V32_LINE_ENTRY_CALLER_STARTUP:
		return !v32_line_startup_is_complete(
			&datapump->line_startup) &&
		       !v32_conditioning_receiver_is_complete(
			       &datapump->conditioning_receiver) &&
		       datapump->conditioning_transmitter.emitted_symbols == 0 &&
		       !datapump->has_previous_receive_point &&
		       datapump->turnaround_s_symbols_remaining == 0 &&
		       !datapump->second_conditioning_complete &&
		       datapump->line_startup.state !=
			       V32_LINE_STARTUP_FAILED &&
		       datapump->direct_qam_qualifying_samples <=
			       V32_DIRECT_QAM_QUALIFY_SAMPLES &&
		       (datapump->line_startup.state ==
					V32_LINE_WAIT_ANSWER_TONE ||
			datapump->direct_qam_qualifying_samples == 0);
	case V32_LINE_ENTRY_CALLER_RECEIVE_CONDITIONING:
		return v32_line_startup_is_complete(
			       &datapump->line_startup) &&
		       !v32_conditioning_receiver_is_complete(
			       &datapump->conditioning_receiver) &&
		       datapump->conditioning_transmitter.emitted_symbols == 0 &&
		       !datapump->has_previous_receive_point &&
		       datapump->turnaround_s_symbols_remaining ==
			       turnaround_s_symbols &&
		       !datapump->second_conditioning_complete &&
		       (!datapump->conditioning_carrier_detected ||
			datapump->conditioning_silence_samples ==
				V32_CONDITIONING_SILENCE_SAMPLES) &&
		       datapump->direct_qam_qualifying_samples == 0;
	case V32_LINE_ENTRY_CALLER_RECEIVE_RATE_OFFER:
		return v32_line_startup_is_complete(
			       &datapump->line_startup) &&
		       v32_conditioning_receiver_is_complete(
			       &datapump->conditioning_receiver) &&
		       qam_gain_is_valid(
			       &datapump->conditioning_gain) &&
		       datapump->has_previous_receive_point &&
		       datapump->conditioning_transmitter.emitted_symbols == 0 &&
		       datapump->turnaround_s_symbols_remaining ==
			       turnaround_s_symbols &&
		       !datapump->second_conditioning_complete &&
		       datapump->startup.phase ==
			       V32_CALLER_WAIT_OFFER &&
		       datapump->direct_qam_qualifying_samples == 0;
	case V32_LINE_ENTRY_CALLER_TRANSMIT_CONDITIONING:
		return v32_line_startup_is_complete(
			       &datapump->line_startup) &&
		       v32_conditioning_receiver_is_complete(
			       &datapump->conditioning_receiver) &&
		       qam_gain_is_valid(
			       &datapump->conditioning_gain) &&
		       datapump->has_previous_receive_point &&
		       !v32_conditioning_transmitter_is_complete(
			       &datapump->conditioning_transmitter) &&
		       !datapump->second_conditioning_complete &&
		       datapump->startup.phase ==
			       V32_CALLER_SEND_RESPONSE &&
		       datapump->direct_qam_qualifying_samples == 0;
	case V32_LINE_ENTRY_CALLER_RATE_EXCHANGE:
		return v32_line_startup_is_complete(
			       &datapump->line_startup) &&
		       v32_conditioning_receiver_is_complete(
			       &datapump->conditioning_receiver) &&
		       qam_gain_is_valid(
			       &datapump->conditioning_gain) &&
		       datapump->has_previous_receive_point &&
		       v32_conditioning_transmitter_is_complete(
			       &datapump->conditioning_transmitter) &&
		       datapump->turnaround_s_symbols_remaining == 0 &&
		       datapump->startup.phase !=
			       V32_CALLER_WAIT_OFFER &&
		       datapump->direct_qam_qualifying_samples == 0;
	case V32_LINE_ENTRY_CALLER_RECEIVE_SECOND_CONDITIONING:
		return v32_line_startup_is_complete(
			       &datapump->line_startup) &&
		       !v32_conditioning_receiver_is_complete(
			       &datapump->conditioning_receiver) &&
		       qam_gain_is_valid(
			       &datapump->conditioning_gain) &&
		       !datapump->has_previous_receive_point &&
		       v32_conditioning_transmitter_is_complete(
			       &datapump->conditioning_transmitter) &&
		       datapump->turnaround_s_symbols_remaining == 0 &&
		       !datapump->second_conditioning_complete &&
		       caller_awaits_second_conditioning(&datapump->startup) &&
		       datapump->direct_qam_qualifying_samples == 0;
	case V32_LINE_ENTRY_DIRECT_QAM_COMPATIBILITY:
		return datapump->line_startup.state ==
			       V32_LINE_WAIT_ANSWER_TONE &&
		       datapump->conditioning_transmitter.emitted_symbols == 0 &&
		       datapump->turnaround_s_symbols_remaining == 0 &&
		       !datapump->second_conditioning_complete &&
		       !v32_conditioning_receiver_is_complete(
			       &datapump->conditioning_receiver) &&
		       datapump->direct_qam_qualifying_samples ==
			       V32_DIRECT_QAM_QUALIFY_SAMPLES;
	case V32_LINE_ENTRY_ANSWERER_STARTUP:
	case V32_LINE_ENTRY_ANSWERER_PROTOCOL:
		return false;
	}
	return false;
}

static uint8_t rate_capabilities(const struct modem *modem)
{
	uint8_t rates = 0;

	if (modem == NULL)
		return 0;
	if (modem->min_rate <= V32_4800_BIT_RATE &&
	    modem->max_rate >= V32_4800_BIT_RATE)
		rates |= V32_RATE_CAPABILITY_4800;
	if (modem->min_rate <= V32_7200_BIT_RATE &&
	    modem->max_rate >= V32_7200_BIT_RATE)
		rates |= V32_RATE_CAPABILITY_7200;
	if (modem->min_rate <= V32_9600_BIT_RATE &&
	    modem->max_rate >= V32_9600_BIT_RATE)
		rates |= V32_RATE_CAPABILITY_9600;
	if (modem->min_rate <= V32_12000_BIT_RATE &&
	    modem->max_rate >= V32_12000_BIT_RATE)
		rates |= V32_RATE_CAPABILITY_12000;
	if (modem->min_rate <= V32_14400_BIT_RATE &&
	    modem->max_rate >= V32_14400_BIT_RATE)
		rates |= V32_RATE_CAPABILITY_14400;
	return rates;
}

static uint8_t selected_trellis_data_bits(
	const struct v32_datapump *datapump)
{
	switch (datapump->startup.selected_rate) {
	case V32_RATE_CAPABILITY_7200:
		return V32_7200_DATA_BITS;
	case V32_RATE_CAPABILITY_9600:
		return V32_9600_TRELLIS_DATA_BITS;
	case V32_RATE_CAPABILITY_12000:
		return V32_12000_DATA_BITS;
	case V32_RATE_CAPABILITY_14400:
		return V32_14400_DATA_BITS;
	default:
		return 0;
	}
}

static bool uses_trellis(const struct v32_datapump *datapump)
{
	return selected_trellis_data_bits(datapump) != 0;
}

static bool uses_uncoded_9600(const struct v32_datapump *datapump)
{
	return datapump->startup.selected_rate == V32_RATE_CAPABILITY_9600 &&
	       !uses_trellis(datapump);
}

static bool trellis_receiver_is_valid(
	const struct v32_datapump *datapump)
{
	uint8_t data_bits = selected_trellis_data_bits(datapump);

	return v32_trellis_acquirer_is_valid(
		       &datapump->trellis_acquirer) &&
	       datapump->trellis_acquirer.candidates[0]
			       .decoder.data_bits_per_symbol == data_bits &&
	       datapump->trellis_acquirer.candidates[0]
			       .descrambler.transmitter_role ==
		       remote_role(datapump->caller);
}

static struct v32_datapump *v32_from_dp(struct dp *dp)
{
	struct v32_datapump *datapump;
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
	    !v32_startup_is_valid(&datapump->startup) ||
	    datapump->startup.role != endpoint_role(datapump->caller) ||
	    datapump->startup.supported_rates != rate_capabilities(dp->modem) ||
	    datapump->linked !=
		    v32_startup_is_complete(&datapump->startup) ||
	    datapump->trellis_receiver_initialized !=
		    (trellis_data_bits != 0 && datapump->startup.end_received) ||
	    (datapump->trellis_receiver_initialized &&
	     !trellis_receiver_is_valid(datapump)) ||
	    (datapump->trellis_encoder_initialized &&
	     (trellis_data_bits == 0 ||
	      !v32_trellis_encoder_is_valid(
		      &datapump->trellis_encoder) ||
	      datapump->trellis_encoder.data_bits_per_symbol !=
		      trellis_data_bits)) ||
	    datapump->receiver_9600_initialized !=
		    (uses_uncoded_9600(datapump) &&
		     datapump->startup.end_received) ||
	    (datapump->receiver_9600_initialized &&
	     !v32_9600_receiver_is_valid(
		     &datapump->receiver_9600)) ||
	    datapump->encoder.phase_quadrant >=
		    QPSK_QUADRANT_COUNT ||
	    datapump->decoder.phase_quadrant >=
		    QPSK_QUADRANT_COUNT ||
	    !v32_scrambler_is_valid(&datapump->encoder.scrambler) ||
	    !v32_scrambler_is_valid(
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

static struct dp *create_v32(
	struct modem *modem, enum DP_ID id, int caller, int sample_rate,
	int max_fragment, struct dp_operations *operations)
{
	struct v32_datapump *datapump;
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
	if (qam_modulator_init(
		    &datapump->modulator, &v32_channel,
		    V32_TRANSMIT_AMPLITUDE) != QAM_OK ||
	    qam_demodulator_init(
		    &datapump->demodulator, &v32_channel) != QAM_OK ||
	    v32_symbol_encoder_init(
		    &datapump->encoder, local_role(is_caller), 0) !=
		    V32_FRAMING_OK ||
	    v32_symbol_decoder_init(
		    &datapump->decoder, remote_role(is_caller), 0) !=
		    V32_FRAMING_OK ||
	    v32_startup_init(
		    &datapump->startup, endpoint_role(is_caller),
		    rates) !=
		    V32_STARTUP_OK ||
	    v32_line_startup_init(
		    &datapump->line_startup, V32_TRANSMIT_AMPLITUDE) !=
		    V32_LINE_STARTUP_OK ||
	    v32_answerer_line_startup_init(
		    &datapump->answerer_line_startup,
		    V32_TRANSMIT_AMPLITUDE) != V32_LINE_STARTUP_OK ||
	    v32_conditioning_transmitter_init(
		    &datapump->conditioning_transmitter, local_role(is_caller),
		    V32_CONDITIONING_TRN_MIN_SYMBOLS) !=
		    V32_CONDITIONING_OK ||
	    v32_conditioning_receiver_init(
		    &datapump->conditioning_receiver, remote_role(is_caller)) !=
		    V32_CONDITIONING_RECEIVER_OK ||
	    v32_4800_constellation_point(
		    0, &datapump->transmit_point) !=
		    V32_CONSTELLATION_OK) {
		free(datapump);
		return NULL;
	}
	datapump->caller = is_caller;
	datapump->line_entry = is_caller ?
				 V32_LINE_ENTRY_CALLER_STARTUP :
				 V32_LINE_ENTRY_ANSWERER_STARTUP;
	datapump->training_timeout_samples =
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

static int delete_v32(struct dp *dp)
{
	struct v32_datapump *datapump = v32_from_dp(dp);

	if (datapump == NULL || v32_operations.use_count <= 0)
		return -1;
	v32_operations.use_count--;
	datapump->dp.dp_data = NULL;
	free(datapump);
	return 0;
}

static enum v32_framing_result encode_4800_transmit_quadrant(
	struct v32_symbol_encoder *encoder,
	enum v32_startup_output output, uint8_t input,
	uint8_t *quadrant)
{
	if (output == V32_STARTUP_TRN_DIBIT)
		return v32_trn_symbol_encode(
			encoder, input, quadrant);
	if (output == V32_STARTUP_DIFFERENTIAL_DIBIT ||
	    output == V32_STARTUP_SELECTED_RATE_MARK ||
	    output == V32_STARTUP_DATA)
		return v32_4800_symbol_encode(
			encoder, input, quadrant);
	return V32_FRAMING_INVALID_ARGUMENT;
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
	struct v32_datapump *datapump, uint8_t input)
{
	struct v32_trellis_encoder next_encoder;
	struct v32_scrambler next_scrambler =
		datapump->encoder.scrambler;
	struct qam_point next_point;
	uint8_t data_bits = selected_trellis_data_bits(datapump);
	uint8_t scrambled;
	uint8_t signal_symbol;

	if (data_bits == 0)
		return -1;
	if (datapump->trellis_encoder_initialized) {
		next_encoder = datapump->trellis_encoder;
	} else if (v32_trellis_encoder_init(
			   &next_encoder, data_bits,
			   V32_TRELLIS_INITIAL_DIFFERENTIAL_OUTPUT) !=
		   V32_TRELLIS_OK) {
		return -1;
	}
	if (v32_scramble_group(
		    &next_scrambler, input, data_bits,
		    &scrambled) != V32_FRAMING_OK ||
	    v32_trellis_encode(
		    &next_encoder, scrambled, &signal_symbol) !=
		    V32_TRELLIS_OK ||
	    v32_trellis_constellation_point(
		    data_bits, signal_symbol, &next_point) !=
		    V32_CONSTELLATION_OK)
		return -1;
	datapump->encoder.scrambler = next_scrambler;
	datapump->trellis_encoder = next_encoder;
	datapump->transmit_point = next_point;
	datapump->trellis_encoder_initialized = true;
	return 0;
}

static int next_transmit_point(
	struct v32_datapump *datapump, bool *linked)
{
	uint8_t input = 3;
	uint8_t symbol;
	uint8_t trellis_data_bits = selected_trellis_data_bits(datapump);
	size_t bit_count = V32_4800_BITS_PER_SYMBOL;
	enum v32_startup_output output;
	enum v32_framing_result framing_result;

	if (v32_startup_next_transmit(
		    &datapump->startup, &output, &input) !=
	    V32_STARTUP_OK)
		return -1;
	if (output == V32_STARTUP_DATA) {
		uint8_t bits[V32_TRELLIS_MAX_DATA_BITS] = {
			1, 1, 1, 1, 1, 1
		};

		if (!datapump->linked) {
			if (enter_link(datapump) != 0)
				return -1;
			*linked = true;
		}
		if (uses_uncoded_9600(datapump))
			bit_count = V32_9600_BITS_PER_SYMBOL;
		else if (trellis_data_bits != 0)
			bit_count = trellis_data_bits;
		if (modem_get_bits(
			    datapump->dp.modem, 1, bits, (int)bit_count) !=
		    (int)bit_count)
			for (size_t index = 0; index < bit_count; ++index)
				bits[index] = 1;
		input = pack_bits(bits, bit_count);
	}
	if (output == V32_STARTUP_SELECTED_RATE_MARK &&
	    uses_uncoded_9600(datapump))
		input = 15;
	if (output == V32_STARTUP_SELECTED_RATE_MARK &&
	    trellis_data_bits != 0)
		input = (uint8_t)((UINT8_C(1) << trellis_data_bits) - 1u);
	if (trellis_data_bits != 0 &&
	    (output == V32_STARTUP_SELECTED_RATE_MARK ||
	     output == V32_STARTUP_DATA)) {
		if (encode_trellis_transmit_point(datapump, input) != 0)
			return -1;
	} else if (uses_uncoded_9600(datapump) &&
	    (output == V32_STARTUP_SELECTED_RATE_MARK ||
	     output == V32_STARTUP_DATA)) {
		framing_result = v32_9600_symbol_encode(
			&datapump->encoder, input, &symbol);
		if (framing_result != V32_FRAMING_OK ||
		    v32_constellation_point(
			    symbol, &datapump->transmit_point) !=
			    V32_CONSTELLATION_OK)
			return -1;
	} else {
		framing_result = encode_4800_transmit_quadrant(
			&datapump->encoder, output, input, &symbol);
		if (framing_result != V32_FRAMING_OK ||
		    v32_4800_constellation_point(
			    symbol, &datapump->transmit_point) !=
			    V32_CONSTELLATION_OK)
			return -1;
	}
	if (!datapump->linked &&
	    v32_startup_is_complete(&datapump->startup)) {
		if (enter_link(datapump) != 0)
			return -1;
		*linked = true;
	}
	return 0;
}

static int advance_answerer_training(
	struct v32_datapump *datapump)
{
	enum v32_startup_output output;
	uint8_t dibit;

	if (v32_startup_next_transmit(
		    &datapump->startup, &output, &dibit) != V32_STARTUP_OK ||
	    output != V32_STARTUP_TRN_DIBIT || dibit != 3)
		return -1;
	return 0;
}

static int generate_conditioning_sample(
	struct v32_datapump *datapump, int16_t *sample)
{
	bool can_accept = qam_modulator_can_accept_symbol(
		&datapump->modulator);
	bool accepted;

	if (can_accept) {
		uint8_t quadrant;

		if (datapump->turnaround_s_symbols_remaining != 0) {
			quadrant =
				(datapump->turnaround_s_symbols_remaining & 1u) != 0 ?
					V32_SIGNAL_B : V32_SIGNAL_A;
			datapump->turnaround_s_symbols_remaining--;
		} else if (v32_conditioning_transmitter_next(
				   &datapump->conditioning_transmitter, &quadrant) !=
			   V32_CONDITIONING_OK) {
			return -1;
		}
		if (!datapump->caller &&
		    datapump->conditioning_transmitter.emitted_symbols >
			    V32_CONDITIONING_S_SYMBOLS +
				    V32_CONDITIONING_S_BAR_SYMBOLS &&
		    advance_answerer_training(datapump) != 0)
			return -1;
		if (v32_4800_constellation_point(
			    quadrant, &datapump->transmit_point) !=
		    V32_CONSTELLATION_OK)
			return -1;
	}
	if (qam_modulator_sample(
		    &datapump->modulator, &datapump->transmit_point, sample,
		    &accepted) != QAM_OK ||
	    accepted != can_accept)
		return -1;
	if (accepted &&
	    v32_conditioning_transmitter_is_complete(
		    &datapump->conditioning_transmitter)) {
		if (v32_conditioning_transmitter_framing_state(
			    &datapump->conditioning_transmitter,
			    &datapump->encoder) != V32_CONDITIONING_OK)
			return -1;
		if (datapump->caller)
			datapump->line_entry =
				V32_LINE_ENTRY_CALLER_RATE_EXCHANGE;
	}
	return 0;
}

static int generate_sample(
	struct v32_datapump *datapump, int16_t *sample,
	bool *linked)
{
	bool can_accept;
	bool accepted;

	if (datapump->line_entry ==
	    V32_LINE_ENTRY_CALLER_TRANSMIT_CONDITIONING)
		return generate_conditioning_sample(datapump, sample);
	if (!datapump->caller &&
	    (datapump->startup.phase == V32_ANSWERER_TRAIN ||
	     datapump->startup.phase == V32_ANSWERER_RETRAIN))
		return generate_conditioning_sample(datapump, sample);

	if (datapump->caller &&
	    (datapump->line_entry ==
		     V32_LINE_ENTRY_CALLER_RECEIVE_CONDITIONING ||
	     datapump->line_entry ==
		     V32_LINE_ENTRY_CALLER_RECEIVE_RATE_OFFER)) {
		*sample = 0;
		return 0;
	}
	can_accept = qam_modulator_can_accept_symbol(
		&datapump->modulator);

	if ((can_accept && next_transmit_point(datapump, linked) != 0) ||
	    qam_modulator_sample(
		    &datapump->modulator, &datapump->transmit_point, sample,
		    &accepted) != QAM_OK ||
	    accepted != can_accept)
		return -1;
	return 0;
}

static int process_caller_line_startup(
	struct v32_datapump *datapump, int16_t receive_sample,
	int16_t *transmit_sample)
{
	struct qam_point ignored_point;
	struct qam_measurement measurement;
	enum v32_line_startup_result result;
	bool ignored_ready;
	bool complete;

	result = v32_line_startup_process(
		&datapump->line_startup, receive_sample, transmit_sample,
		&complete);
	if (result != V32_LINE_STARTUP_OK)
		return -1;
	if (complete) {
		if (!caller_turnaround_s_symbols(
			    &datapump->line_startup,
			    &datapump->turnaround_s_symbols_remaining))
			return -1;
		if (qam_demodulator_reset(&datapump->demodulator) !=
		    QAM_OK)
			return -1;
		datapump->has_previous_receive_point = false;
		datapump->line_entry =
			V32_LINE_ENTRY_CALLER_RECEIVE_CONDITIONING;
		return 0;
	}
	if (datapump->line_startup.state !=
	    V32_LINE_WAIT_ANSWER_TONE) {
		datapump->direct_qam_qualifying_samples = 0;
		return 0;
	}
	if (qam_demodulator_sample(
		    &datapump->demodulator, receive_sample, &ignored_point,
		    &ignored_ready) != QAM_OK)
		return -1;
	measurement = qam_demodulator_measure(
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

static int process_answerer_line_startup(
	struct v32_datapump *datapump, int16_t receive_sample,
	int16_t *transmit_sample)
{
	bool complete;

	if (v32_answerer_line_startup_process(
		    &datapump->answerer_line_startup, receive_sample,
		    transmit_sample, &complete) != V32_LINE_STARTUP_OK)
		return -1;
	if (!complete)
		return 0;
	if (qam_demodulator_reset(&datapump->demodulator) != QAM_OK)
		return -1;
	datapump->has_previous_receive_point = false;
	datapump->line_entry = V32_LINE_ENTRY_ANSWERER_PROTOCOL;
	return 0;
}

static unsigned int selected_bit_rate(
	const struct v32_datapump *datapump)
{
	switch (datapump->startup.selected_rate) {
	case V32_RATE_CAPABILITY_4800:
		return V32_4800_BIT_RATE;
	case V32_RATE_CAPABILITY_7200:
		return V32_7200_BIT_RATE;
	case V32_RATE_CAPABILITY_9600:
		return V32_9600_BIT_RATE;
	case V32_RATE_CAPABILITY_12000:
		return V32_12000_BIT_RATE;
	case V32_RATE_CAPABILITY_14400:
		return V32_14400_BIT_RATE;
	default:
		return 0;
	}
}

static int enter_link(struct v32_datapump *datapump)
{
	unsigned int bit_rate = selected_bit_rate(datapump);

	if (bit_rate == 0 ||
	    (uses_trellis(datapump) &&
	     (!datapump->trellis_receiver_initialized ||
	      !datapump->trellis_encoder_initialized)) ||
	    (uses_uncoded_9600(datapump) &&
	     !datapump->receiver_9600_initialized))
		return -1;
	if (uses_trellis(datapump)) {
		if (v32_trellis_acquirer_select(
			    &datapump->trellis_acquirer) !=
		    V32_TRELLIS_ACQUIRER_OK)
			return -1;
	}
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
	struct v32_datapump *datapump, uint8_t value,
	size_t bit_count)
{
	uint8_t bits[V32_TRELLIS_MAX_DATA_BITS];

	for (size_t index = 0; index < bit_count; ++index)
		bits[index] = (uint8_t)((value >> (bit_count - index - 1u)) & 1u);
	if (datapump->dp.modem->put_bits != NULL)
		(void)modem_put_bits(
			datapump->dp.modem, 1, bits, (int)bit_count);
}

static int initialize_trellis_receiver(
	struct v32_datapump *datapump,
	const struct qam_point *reference_point)
{
	struct v32_trellis_acquirer acquirer;
	uint8_t data_bits = selected_trellis_data_bits(datapump);

	if (data_bits == 0 ||
	    v32_trellis_acquirer_init(
		    &acquirer, &datapump->decoder, reference_point,
		    data_bits) != V32_TRELLIS_ACQUIRER_OK)
		return -1;
	datapump->trellis_acquirer = acquirer;
	datapump->trellis_receiver_initialized = true;
	return 0;
}

static int reset_conditioning_receiver(
	struct v32_datapump *datapump, bool carrier_detected)
{
	struct v32_conditioning_receiver receiver;
	struct qam_demodulator demodulator = datapump->demodulator;

	if (v32_conditioning_receiver_init(
		    &receiver, remote_role(datapump->caller)) !=
		    V32_CONDITIONING_RECEIVER_OK ||
	    qam_demodulator_reset(&demodulator) !=
		    QAM_OK)
		return -1;
	datapump->conditioning_receiver = receiver;
	datapump->demodulator = demodulator;
	datapump->has_previous_receive_point = false;
	datapump->conditioning_carrier_detected = carrier_detected;
	datapump->conditioning_silence_samples = 0;
	return 0;
}

static int begin_second_conditioning(
	struct v32_datapump *datapump)
{
	if (reset_conditioning_receiver(datapump, true) != 0)
		return -1;
	datapump->line_entry =
		V32_LINE_ENTRY_CALLER_RECEIVE_SECOND_CONDITIONING;
	return 0;
}

static int track_rate_signal_loss(
	struct v32_datapump *datapump, double line_power)
{
	/* The demodulator's 256-sample power estimator already rejects brief
	 * sample and media gaps before it crosses this threshold. */
	if (datapump->line_entry != V32_LINE_ENTRY_CALLER_RATE_EXCHANGE ||
	    datapump->second_conditioning_complete ||
	    !caller_awaits_second_conditioning(&datapump->startup) ||
	    line_power >= V32_MIN_LINE_POWER)
		return 0;
	return begin_second_conditioning(datapump);
}

static int update_direct_selection_wait(
	struct v32_datapump *datapump)
{
	if (datapump->line_entry != V32_LINE_ENTRY_DIRECT_QAM_COMPATIBILITY ||
	    !datapump->caller ||
	    datapump->startup.phase !=
		    V32_CALLER_SEND_RESPONSE ||
	    datapump->startup.selected_rate != 0 ||
	    datapump->startup.selection_expected) {
		datapump->direct_retrain_dibits = 0;
		return 0;
	}
	if (datapump->direct_retrain_dibits < V32_DIRECT_RETRAIN_DIBITS)
		datapump->direct_retrain_dibits++;
	if (datapump->direct_retrain_dibits != V32_DIRECT_RETRAIN_DIBITS)
		return 0;
	if (v32_startup_expect_selection(&datapump->startup) !=
	    V32_STARTUP_OK)
		return -1;
	datapump->direct_retrain_dibits = 0;
	return 0;
}

static int observe_dibit(
	struct v32_datapump *datapump, uint8_t dibit,
	const struct qam_point *point, bool *linked)
{
	if (datapump->linked) {
		if (uses_uncoded_9600(datapump) || uses_trellis(datapump))
			return -1;
		deliver_bits(
			datapump, dibit, V32_4800_BITS_PER_SYMBOL);
		return 0;
	}
	{
		bool connected;
		bool end_was_received = datapump->startup.end_received;
		enum v32_startup_phase previous_phase =
			datapump->startup.phase;

		if (v32_startup_observe(
			    &datapump->startup, dibit, &connected) !=
			    V32_STARTUP_OK)
			return -1;
		if (!datapump->caller &&
		    previous_phase == V32_ANSWERER_SEND_OFFER &&
		    datapump->startup.phase == V32_ANSWERER_RETRAIN &&
		    v32_conditioning_transmitter_init(
			    &datapump->conditioning_transmitter,
			    V32_ANSWERING_TRANSMITTER,
			    V32_CONDITIONING_TRN_MIN_SYMBOLS) !=
			    V32_CONDITIONING_OK)
			return -1;
		if (update_direct_selection_wait(datapump) != 0)
			return -1;
		if (datapump->line_entry ==
			    V32_LINE_ENTRY_CALLER_RECEIVE_RATE_OFFER &&
		    previous_phase == V32_CALLER_WAIT_OFFER &&
		    datapump->startup.phase ==
			    V32_CALLER_SEND_RESPONSE)
			datapump->line_entry =
				V32_LINE_ENTRY_CALLER_TRANSMIT_CONDITIONING;
		if (datapump->line_entry == V32_LINE_ENTRY_CALLER_RATE_EXCHANGE &&
		    !datapump->second_conditioning_complete &&
		    caller_awaits_second_conditioning(&datapump->startup) &&
		    !datapump->startup.detector.aligned &&
		    begin_second_conditioning(datapump) != 0)
			return -1;
		if (uses_trellis(datapump) && !end_was_received &&
		    datapump->startup.end_received) {
			if (initialize_trellis_receiver(datapump, point) != 0)
				return -1;
		} else if (uses_uncoded_9600(datapump) && !end_was_received &&
			   datapump->startup.end_received) {
			if (v32_9600_receiver_init(
				    &datapump->receiver_9600, &datapump->decoder,
				    point) != V32_9600_RECEIVER_OK)
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
	struct v32_datapump *datapump,
	const struct qam_point *point, bool *linked)
{
	uint8_t data_bits = selected_trellis_data_bits(datapump);
	uint8_t mark = (uint8_t)((UINT8_C(1) << data_bits) - 1u);

	if (!datapump->trellis_receiver_initialized || data_bits == 0)
		return -1;
	if (datapump->linked) {
		uint8_t bits;
		bool ready;

		if (v32_trellis_acquirer_decode(
			    &datapump->trellis_acquirer, point, &bits,
			    &ready) != V32_TRELLIS_ACQUIRER_OK)
			return -1;
		if (ready)
			deliver_bits(datapump, bits, data_bits);
	} else {
		bool connected;
		if (v32_trellis_acquirer_track_known(
			    &datapump->trellis_acquirer, point, mark) !=
		    V32_TRELLIS_ACQUIRER_OK)
			return -1;
		/* B1 confirmation counts the accepted known symbol.  Its decoded
		 * value is intentionally delayed by equalization and traceback. */
		if (v32_startup_observe(
			    &datapump->startup, 3, &connected) !=
		    V32_STARTUP_OK)
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
	struct v32_datapump *datapump,
	const struct qam_point *point, bool *linked)
{
	if (!datapump->receiver_9600_initialized)
		return -1;
	if (datapump->linked) {
		uint8_t bits;
		bool ready;

		if (v32_9600_receiver_decode(
			    &datapump->receiver_9600, point, &bits, &ready) !=
		    V32_9600_RECEIVER_OK)
			return -1;
		if (ready)
			deliver_bits(
				datapump, bits,
				V32_9600_BITS_PER_SYMBOL);
	} else {
		bool connected;

		if (v32_9600_receiver_track_known(
			    &datapump->receiver_9600, point, 15) !=
			    V32_9600_RECEIVER_OK ||
		    v32_startup_observe(
			    &datapump->startup, 3,
			    &connected) != V32_STARTUP_OK)
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
	struct v32_datapump *datapump, int16_t sample,
	bool *linked)
{
	struct qam_point point;
	struct qam_measurement measurement;
	bool ready;

	if (receives_conditioning(datapump->line_entry) &&
	    !datapump->conditioning_carrier_detected) {
		int32_t magnitude = sample < 0 ? -(int32_t)sample : sample;

		if (datapump->conditioning_silence_samples <
		    V32_CONDITIONING_SILENCE_SAMPLES) {
			if (magnitude <= V32_CONDITIONING_SIGNAL_THRESHOLD)
				datapump->conditioning_silence_samples++;
			else
				datapump->conditioning_silence_samples = 0;
			return 0;
		}
		if (magnitude <= V32_CONDITIONING_SIGNAL_THRESHOLD)
			return 0;
		if (qam_demodulator_reset(&datapump->demodulator) != QAM_OK)
			return -1;
		datapump->conditioning_carrier_detected = true;
	}

	if (qam_demodulator_sample(
		    &datapump->demodulator, sample, &point, &ready) !=
	    QAM_OK)
		return -1;
	measurement = qam_demodulator_measure(
		&datapump->demodulator);
	if (track_rate_signal_loss(datapump, measurement.line_power) != 0)
		return -1;
	if (!ready)
		return 0;
	if (receives_conditioning(datapump->line_entry) ||
	    (!datapump->caller &&
	     datapump->line_entry == V32_LINE_ENTRY_ANSWERER_PROTOCOL &&
	     !v32_conditioning_receiver_is_complete(
		     &datapump->conditioning_receiver))) {
		enum v32_line_entry next_entry =
			!datapump->caller ?
				V32_LINE_ENTRY_ANSWERER_PROTOCOL :
			datapump->line_entry ==
				V32_LINE_ENTRY_CALLER_RECEIVE_CONDITIONING ?
				V32_LINE_ENTRY_CALLER_RECEIVE_RATE_OFFER :
				V32_LINE_ENTRY_CALLER_RATE_EXCHANGE;
		bool conditioning_ready;
		bool second_conditioning =
			datapump->line_entry ==
			V32_LINE_ENTRY_CALLER_RECEIVE_SECOND_CONDITIONING;
		double point_power = point.in_phase * point.in_phase +
				     point.quadrature * point.quadrature;

		/* A weak carrier tail can acquire timing and then yield an empty
		 * recovered point in the following gap.  Restart at the next real
		 * conditioning carrier instead of treating loss of signal as corrupt
		 * detector input. */
		if (receives_conditioning(datapump->line_entry) &&
		    point_power <= v32_min_conditioning_point_power) {
			if (reset_conditioning_receiver(datapump, false) != 0)
				return -1;
			return 0;
		}

		if (v32_conditioning_receiver_observe(
			    &datapump->conditioning_receiver, &point,
			    &conditioning_ready) !=
			    V32_CONDITIONING_RECEIVER_OK)
			return -1;
		if (conditioning_ready) {
			if (second_conditioning &&
			    v32_startup_expect_selection(
				    &datapump->startup) !=
				    V32_STARTUP_OK)
				return -1;
			if (v32_conditioning_receiver_framing_state(
				    &datapump->conditioning_receiver,
				    &datapump->decoder,
				    &datapump->conditioning_gain) !=
			    V32_CONDITIONING_RECEIVER_OK)
				return -1;
			datapump->previous_receive_point = point;
			datapump->has_previous_receive_point = true;
			datapump->second_conditioning_complete =
				second_conditioning;
			datapump->conditioning_carrier_detected = false;
			datapump->conditioning_silence_samples = 0;
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

		if (qam_nearest_quarter_turn(
			    &datapump->previous_receive_point, &point,
			    &quarter_turns) != QAM_OK)
			return -1;
		quadrant = (uint8_t)((datapump->decoder.phase_quadrant +
				      quarter_turns) &
				     3u);
		if (v32_4800_symbol_decode(
			    &datapump->decoder, quadrant, &dibit) !=
			    V32_FRAMING_OK ||
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
	const struct qam_measurement *measurement,
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
	struct v32_datapump *datapump = v32_from_dp(dp);
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
		if (datapump->line_entry == V32_LINE_ENTRY_ANSWERER_STARTUP) {
			if (process_answerer_line_startup(
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
	measurement = qam_demodulator_measure(
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

int v32_datapump_register(void)
{
	return modem_dp_register(DP_V32, &v32_operations);
}

int v32_datapump_unregister(void)
{
	return modem_dp_deregister(DP_V32, &v32_operations);
}

bool v32_datapump_is_busy(void)
{
	return v32_operations.use_count != 0;
}
