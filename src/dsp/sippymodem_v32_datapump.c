// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_v32_datapump.h"

#include "modem.h"
#include "modem_param.h"
#include "sippymodem_qam.h"
#include "sippymodem_v32_9600_receiver.h"
#include "sippymodem_v32_constellation.h"
#include "sippymodem_v32_framing.h"
#include "sippymodem_v32_startup.h"
#include "sippymodem_v32_trellis.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* This four-wire media path begins directly with role-specific TRN and rate
 * exchange. Initial tones, S sequences and two-wire echo cancellation remain
 * follow-up work. */
enum {
	V32_SYMBOL_RATE = 2400,
	V32_CARRIER_HZ = 1800,
	V32_PULSE_SPAN_SYMBOLS = 6,
	V32_TRANSMIT_AMPLITUDE = 12000,
	V32_CARRIER_LOSS_SAMPLES = MODEM_RATE / 5,
	V32_MIN_LINE_POWER = 1000000,
};

static const struct sippymodem_qam_channel v32_channel = {
	.sample_rate = MODEM_RATE,
	.symbol_rate_numerator = V32_SYMBOL_RATE,
	.symbol_rate_denominator = 1,
	.carrier_frequency_hz = V32_CARRIER_HZ,
	.rolloff = 0.25,
	.pulse_span_symbols = V32_PULSE_SPAN_SYMBOLS,
};

static const double V32_7200_MIN_POINT_POWER = 1e-12;
static const double V32_7200_CHANNEL_ADAPTATION = 1.0 / 32.0;

struct sippymodem_v32_datapump {
	struct dp dp;
	struct sippymodem_qam_modulator modulator;
	struct sippymodem_qam_demodulator demodulator;
	struct sippymodem_v32_symbol_encoder encoder;
	struct sippymodem_v32_symbol_decoder decoder;
	struct sippymodem_v32_trellis_encoder encoder_7200;
	struct sippymodem_v32_trellis_encoder known_encoder_7200;
	struct sippymodem_v32_7200_decoder decoder_7200;
	struct sippymodem_v32_9600_receiver receiver_9600;
	struct sippymodem_v32_startup startup;
	struct sippymodem_qam_point transmit_point;
	struct sippymodem_qam_point previous_receive_point;
	struct sippymodem_qam_point channel_gain_7200;
	uint64_t training_samples;
	uint64_t training_timeout_samples;
	uint32_t missing_carrier_samples;
	uint8_t receive_7200_link_symbols;
	bool has_previous_receive_point;
	bool encoder_7200_initialized;
	bool receiver_7200_initialized;
	bool receiver_9600_initialized;
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

static bool point_is_finite(const struct sippymodem_qam_point *point)
{
	return point != NULL && isfinite(point->in_phase) &&
	       isfinite(point->quadrature);
}

static bool point_power(
	const struct sippymodem_qam_point *point, double *power)
{
	double value;

	if (!point_is_finite(point) || power == NULL)
		return false;
	value = point->in_phase * point->in_phase +
		point->quadrature * point->quadrature;
	if (!isfinite(value) || value <= V32_7200_MIN_POINT_POWER)
		return false;
	*power = value;
	return true;
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
	return rates;
}

static bool uses_7200(const struct sippymodem_v32_datapump *datapump)
{
	return datapump->startup.selected_rate ==
	       SIPPYMODEM_V32_RATE_CAPABILITY_7200;
}

static bool uses_9600(const struct sippymodem_v32_datapump *datapump)
{
	return datapump->startup.selected_rate ==
	       SIPPYMODEM_V32_RATE_CAPABILITY_9600;
}

static bool receiver_7200_is_valid(
	const struct sippymodem_v32_datapump *datapump)
{
	double gain_power;

	return sippymodem_v32_7200_decoder_is_valid(&datapump->decoder_7200) &&
	       sippymodem_v32_trellis_encoder_is_valid(
		       &datapump->known_encoder_7200) &&
	       datapump->known_encoder_7200.data_bits_per_symbol ==
		       SIPPYMODEM_V32_7200_DATA_BITS &&
	       point_power(&datapump->channel_gain_7200, &gain_power) &&
	       datapump->receive_7200_link_symbols <=
		       SIPPYMODEM_V32_TRELLIS_TRACEBACK_SYMBOLS;
}

static struct sippymodem_v32_datapump *v32_from_dp(struct dp *dp)
{
	struct sippymodem_v32_datapump *datapump;

	if (dp == NULL || dp->id != DP_V32 || dp->op != &v32_operations ||
	    dp->modem == NULL || dp->dp_data == NULL)
		return NULL;
	datapump = dp->dp_data;
	if (&datapump->dp != dp ||
	    datapump->training_timeout_samples == 0 ||
	    datapump->training_samples > datapump->training_timeout_samples ||
	    datapump->missing_carrier_samples > V32_CARRIER_LOSS_SAMPLES ||
	    !sippymodem_v32_startup_is_valid(&datapump->startup) ||
	    datapump->startup.role != endpoint_role(datapump->caller) ||
	    datapump->startup.supported_rates != rate_capabilities(dp->modem) ||
	    datapump->linked !=
		    sippymodem_v32_startup_is_complete(&datapump->startup) ||
	    datapump->receiver_7200_initialized !=
		    (uses_7200(datapump) && datapump->startup.end_received) ||
	    (datapump->receiver_7200_initialized &&
	     !receiver_7200_is_valid(datapump)) ||
	    (datapump->encoder_7200_initialized &&
	     (!uses_7200(datapump) ||
	      !sippymodem_v32_trellis_encoder_is_valid(
		      &datapump->encoder_7200) ||
	      datapump->encoder_7200.data_bits_per_symbol !=
		      SIPPYMODEM_V32_7200_DATA_BITS)) ||
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
	    (datapump->linked && uses_7200(datapump) &&
	     !datapump->encoder_7200_initialized) ||
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
	    sippymodem_v32_4800_constellation_point(
		    0, &datapump->transmit_point) !=
		    SIPPYMODEM_V32_CONSTELLATION_OK) {
		free(datapump);
		return NULL;
	}
	datapump->caller = is_caller;
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
		packed = (uint8_t)((packed << 1) | (bits[index] & 1u));
	return packed;
}

static int encode_7200_transmit_point(
	struct sippymodem_v32_datapump *datapump, uint8_t input)
{
	struct sippymodem_v32_trellis_encoder next_encoder;
	struct sippymodem_v32_scrambler next_scrambler =
		datapump->encoder.scrambler;
	struct sippymodem_qam_point next_point;
	uint8_t scrambled;
	uint8_t signal_symbol;

	if (datapump->encoder_7200_initialized) {
		next_encoder = datapump->encoder_7200;
	} else if (sippymodem_v32_trellis_encoder_init(
			   &next_encoder, SIPPYMODEM_V32_7200_DATA_BITS, 0) !=
		   SIPPYMODEM_V32_TRELLIS_OK) {
		return -1;
	}
	if (sippymodem_v32_scramble_group(
		    &next_scrambler, input, SIPPYMODEM_V32_7200_DATA_BITS,
		    &scrambled) != SIPPYMODEM_V32_FRAMING_OK ||
	    sippymodem_v32_trellis_encode(
		    &next_encoder, scrambled, &signal_symbol) !=
		    SIPPYMODEM_V32_TRELLIS_OK ||
	    sippymodem_v32_7200_constellation_point(
		    signal_symbol, &next_point) !=
		    SIPPYMODEM_V32_CONSTELLATION_OK)
		return -1;
	datapump->encoder.scrambler = next_scrambler;
	datapump->encoder_7200 = next_encoder;
	datapump->transmit_point = next_point;
	datapump->encoder_7200_initialized = true;
	return 0;
}

static int next_transmit_point(
	struct sippymodem_v32_datapump *datapump, bool *linked)
{
	uint8_t bits[SIPPYMODEM_V32_9600_BITS_PER_SYMBOL] = { 1, 1, 1, 1 };
	uint8_t input = 3;
	uint8_t symbol;
	size_t bit_count = SIPPYMODEM_V32_4800_BITS_PER_SYMBOL;
	enum sippymodem_v32_startup_output output;
	enum sippymodem_v32_framing_result framing_result;

	if (sippymodem_v32_startup_next_transmit(
		    &datapump->startup, &output, &input) !=
	    SIPPYMODEM_V32_STARTUP_OK)
		return -1;
	if (output == SIPPYMODEM_V32_STARTUP_DATA) {
		if (!datapump->linked) {
			if (enter_link(datapump) != 0)
				return -1;
			*linked = true;
		}
		if (uses_9600(datapump))
			bit_count = SIPPYMODEM_V32_9600_BITS_PER_SYMBOL;
		else if (uses_7200(datapump))
			bit_count = SIPPYMODEM_V32_7200_DATA_BITS;
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
	    uses_7200(datapump))
		input = 7;
	if (uses_7200(datapump) &&
	    (output == SIPPYMODEM_V32_STARTUP_SELECTED_RATE_MARK ||
	     output == SIPPYMODEM_V32_STARTUP_DATA)) {
		if (encode_7200_transmit_point(datapump, input) != 0)
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

static int generate_sample(
	struct sippymodem_v32_datapump *datapump, int16_t *sample,
	bool *linked)
{
	bool can_accept = sippymodem_qam_modulator_can_accept_symbol(
		&datapump->modulator);
	bool accepted;

	if ((can_accept && next_transmit_point(datapump, linked) != 0) ||
	    sippymodem_qam_modulator_sample(
		    &datapump->modulator, &datapump->transmit_point, sample,
		    &accepted) != SIPPYMODEM_QAM_OK ||
	    accepted != can_accept)
		return -1;
	return 0;
}

static unsigned int selected_bit_rate(
	const struct sippymodem_v32_datapump *datapump)
{
	if (uses_9600(datapump))
		return SIPPYMODEM_V32_9600_BIT_RATE;
	if (uses_7200(datapump))
		return SIPPYMODEM_V32_7200_BIT_RATE;
	if (datapump->startup.selected_rate ==
	    SIPPYMODEM_V32_RATE_CAPABILITY_4800)
		return SIPPYMODEM_V32_4800_BIT_RATE;
	return 0;
}

static int enter_link(struct sippymodem_v32_datapump *datapump)
{
	unsigned int bit_rate = selected_bit_rate(datapump);

	if (bit_rate == 0 ||
	    (uses_7200(datapump) &&
	     (!datapump->receiver_7200_initialized ||
	      !datapump->encoder_7200_initialized)) ||
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
	uint8_t bits[SIPPYMODEM_V32_9600_BITS_PER_SYMBOL];

	for (size_t index = 0; index < bit_count; ++index)
		bits[index] = (uint8_t)((value >> (bit_count - index - 1u)) & 1u);
	if (datapump->dp.modem->put_bits != NULL)
		(void)modem_put_bits(
			datapump->dp.modem, 1, bits, (int)bit_count);
}

static struct sippymodem_qam_point divide_by_complex(
	const struct sippymodem_qam_point *numerator,
	const struct sippymodem_qam_point *denominator,
	double denominator_power)
{
	return (struct sippymodem_qam_point){
		.in_phase =
			(numerator->in_phase * denominator->in_phase +
			 numerator->quadrature * denominator->quadrature) /
			denominator_power,
		.quadrature =
			(numerator->quadrature * denominator->in_phase -
			 numerator->in_phase * denominator->quadrature) /
			denominator_power,
	};
}

static bool update_7200_channel_gain(
	struct sippymodem_qam_point *gain,
	const struct sippymodem_qam_point *received,
	const struct sippymodem_qam_point *transmitted)
{
	struct sippymodem_qam_point observed;
	double gain_power;
	double received_power;
	double transmitted_power;

	if (!point_power(gain, &gain_power) ||
	    !point_power(received, &received_power) ||
	    !point_power(transmitted, &transmitted_power))
		return false;
	observed = divide_by_complex(
		received, transmitted, transmitted_power);
	gain->in_phase += (observed.in_phase - gain->in_phase) *
			  V32_7200_CHANNEL_ADAPTATION;
	gain->quadrature += (observed.quadrature - gain->quadrature) *
			    V32_7200_CHANNEL_ADAPTATION;
	return point_power(gain, &gain_power);
}

static bool equalize_7200_point(
	const struct sippymodem_qam_point *gain,
	const struct sippymodem_qam_point *received,
	struct sippymodem_qam_point *equalized)
{
	double gain_power;

	if (!point_power(gain, &gain_power) || !point_is_finite(received) ||
	    equalized == NULL)
		return false;
	*equalized = divide_by_complex(received, gain, gain_power);
	return point_is_finite(equalized);
}

static int initialize_7200_receiver(
	struct sippymodem_v32_datapump *datapump,
	const struct sippymodem_qam_point *reference_point)
{
	struct sippymodem_v32_trellis_encoder known_encoder;
	struct sippymodem_v32_7200_decoder decoder;
	struct sippymodem_qam_point ideal;
	struct sippymodem_qam_point gain;
	double ideal_power;
	double reference_power;

	if (!point_power(reference_point, &reference_power) ||
	    sippymodem_v32_4800_constellation_point(
		    datapump->decoder.phase_quadrant, &ideal) !=
		    SIPPYMODEM_V32_CONSTELLATION_OK ||
	    !point_power(&ideal, &ideal_power) ||
	    sippymodem_v32_trellis_encoder_init(
		    &known_encoder, SIPPYMODEM_V32_7200_DATA_BITS, 0) !=
		    SIPPYMODEM_V32_TRELLIS_OK ||
	    sippymodem_v32_7200_decoder_init(&decoder, 0) !=
		    SIPPYMODEM_V32_TRELLIS_OK)
		return -1;
	gain = divide_by_complex(reference_point, &ideal, ideal_power);
	if (!point_power(&gain, &ideal_power))
		return -1;
	datapump->known_encoder_7200 = known_encoder;
	datapump->decoder_7200 = decoder;
	datapump->channel_gain_7200 = gain;
	datapump->receive_7200_link_symbols = 0;
	datapump->receiver_7200_initialized = true;
	return 0;
}

static int track_known_7200_point(
	struct sippymodem_v32_datapump *datapump,
	const struct sippymodem_qam_point *point, uint8_t input)
{
	struct sippymodem_v32_scrambler descrambler =
		datapump->decoder.descrambler;
	struct sippymodem_v32_trellis_encoder known_encoder =
		datapump->known_encoder_7200;
	struct sippymodem_v32_7200_decoder decoder =
		datapump->decoder_7200;
	struct sippymodem_qam_point gain = datapump->channel_gain_7200;
	struct sippymodem_qam_point equalized;
	struct sippymodem_qam_point transmitted;
	uint8_t decoded = 0;
	uint8_t scrambled;
	uint8_t signal_symbol;
	bool ready;

	if (!datapump->receiver_7200_initialized ||
	    sippymodem_v32_scramble_group(
		    &descrambler, input, SIPPYMODEM_V32_7200_DATA_BITS,
		    &scrambled) != SIPPYMODEM_V32_FRAMING_OK ||
	    sippymodem_v32_trellis_encode(
		    &known_encoder, scrambled, &signal_symbol) !=
		    SIPPYMODEM_V32_TRELLIS_OK ||
	    sippymodem_v32_7200_constellation_point(
		    signal_symbol, &transmitted) !=
		    SIPPYMODEM_V32_CONSTELLATION_OK ||
	    !update_7200_channel_gain(&gain, point, &transmitted) ||
	    !equalize_7200_point(&gain, point, &equalized) ||
	    sippymodem_v32_7200_decode(
		    &decoder, &equalized, &decoded, &ready) !=
		    SIPPYMODEM_V32_TRELLIS_OK)
		return -1;
	datapump->decoder.descrambler = descrambler;
	datapump->known_encoder_7200 = known_encoder;
	datapump->decoder_7200 = decoder;
	datapump->channel_gain_7200 = gain;
	return 0;
}

static int decode_7200_point(
	struct sippymodem_v32_datapump *datapump,
	const struct sippymodem_qam_point *point, uint8_t *output, bool *ready)
{
	struct sippymodem_v32_scrambler descrambler =
		datapump->decoder.descrambler;
	struct sippymodem_v32_7200_decoder decoder =
		datapump->decoder_7200;
	struct sippymodem_qam_point equalized;
	uint8_t link_symbols = datapump->receive_7200_link_symbols;
	uint8_t decoded = 0;
	uint8_t scrambled = 0;
	bool trellis_ready;
	bool payload_ready;

	if (!datapump->receiver_7200_initialized || output == NULL ||
	    ready == NULL ||
	    !equalize_7200_point(
		    &datapump->channel_gain_7200, point, &equalized) ||
	    sippymodem_v32_7200_decode(
		    &decoder, &equalized, &scrambled, &trellis_ready) !=
		    SIPPYMODEM_V32_TRELLIS_OK)
		return -1;
	if (link_symbols < SIPPYMODEM_V32_TRELLIS_TRACEBACK_SYMBOLS)
		link_symbols++;
	payload_ready =
		trellis_ready &&
		link_symbols == SIPPYMODEM_V32_TRELLIS_TRACEBACK_SYMBOLS;
	if (payload_ready &&
	    sippymodem_v32_descramble_group(
		    &descrambler, scrambled, SIPPYMODEM_V32_7200_DATA_BITS,
		    &decoded) != SIPPYMODEM_V32_FRAMING_OK)
		return -1;
	datapump->decoder.descrambler = descrambler;
	datapump->decoder_7200 = decoder;
	datapump->receive_7200_link_symbols = link_symbols;
	*ready = payload_ready;
	if (payload_ready)
		*output = decoded;
	return 0;
}

static int observe_dibit(
	struct sippymodem_v32_datapump *datapump, uint8_t dibit,
	const struct sippymodem_qam_point *point, bool *linked)
{
	if (datapump->linked) {
		if (uses_9600(datapump))
			return -1;
		deliver_bits(
			datapump, dibit, SIPPYMODEM_V32_4800_BITS_PER_SYMBOL);
		return 0;
	}
	{
		bool connected;
		bool end_was_received = datapump->startup.end_received;

		if (sippymodem_v32_startup_observe(
			    &datapump->startup, dibit, &connected) !=
			    SIPPYMODEM_V32_STARTUP_OK)
			return -1;
		if (uses_7200(datapump) && !end_was_received &&
		    datapump->startup.end_received) {
			if (initialize_7200_receiver(datapump, point) != 0)
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

static int observe_7200_point(
	struct sippymodem_v32_datapump *datapump,
	const struct sippymodem_qam_point *point, bool *linked)
{
	if (!datapump->receiver_7200_initialized)
		return -1;
	if (datapump->linked) {
		uint8_t bits;
		bool ready;

		if (decode_7200_point(datapump, point, &bits, &ready) != 0)
			return -1;
		if (ready)
			deliver_bits(
				datapump, bits, SIPPYMODEM_V32_7200_DATA_BITS);
	} else {
		bool connected;

		if (track_known_7200_point(datapump, point, 7) != 0 ||
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

static int observe_9600_point(
	struct sippymodem_v32_datapump *datapump,
	const struct sippymodem_qam_point *point, bool *linked)
{
	if (!datapump->receiver_9600_initialized)
		return -1;
	if (datapump->linked) {
		uint8_t bits;

		if (sippymodem_v32_9600_receiver_decode(
			    &datapump->receiver_9600, point, &bits) !=
		    SIPPYMODEM_V32_9600_RECEIVER_OK)
			return -1;
		deliver_bits(
			datapump, bits, SIPPYMODEM_V32_9600_BITS_PER_SYMBOL);
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
	bool ready;

	if (sippymodem_qam_demodulator_sample(
		    &datapump->demodulator, sample, &point, &ready) !=
	    SIPPYMODEM_QAM_OK)
		return -1;
	if (!ready)
		return 0;
	if (datapump->receiver_7200_initialized) {
		if (observe_7200_point(datapump, &point, linked) != 0)
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
