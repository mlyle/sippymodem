// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_v32_datapump.h"

#include "modem.h"
#include "modem_param.h"
#include "sippymodem_qam.h"
#include "sippymodem_v32_framing.h"
#include "sippymodem_v32_startup.h"

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

struct sippymodem_v32_datapump {
	struct dp dp;
	struct sippymodem_qam_modulator modulator;
	struct sippymodem_qam_demodulator demodulator;
	struct sippymodem_v32_symbol_encoder encoder;
	struct sippymodem_v32_symbol_decoder decoder;
	struct sippymodem_v32_startup startup;
	struct sippymodem_qam_point transmit_point;
	struct sippymodem_qam_point previous_receive_point;
	uint64_t training_samples;
	uint64_t training_timeout_samples;
	uint32_t missing_carrier_samples;
	bool has_previous_receive_point;
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
	.name = "sippymodem V.32 4800",
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
	    datapump->linked !=
		    sippymodem_v32_startup_is_complete(&datapump->startup) ||
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

static bool rate_is_selected(const struct modem *modem)
{
	return modem != NULL && modem->min_rate <= SIPPYMODEM_V32_BIT_RATE &&
	       modem->max_rate >= SIPPYMODEM_V32_BIT_RATE;
}

static struct sippymodem_qam_point quadrant_point(uint8_t quadrant)
{
	switch (quadrant & 3u) {
	case 0:
		return (struct sippymodem_qam_point){ 1.0, 0.0 };
	case 1:
		return (struct sippymodem_qam_point){ 0.0, 1.0 };
	case 2:
		return (struct sippymodem_qam_point){ -1.0, 0.0 };
	default:
		return (struct sippymodem_qam_point){ 0.0, -1.0 };
	}
}

static struct dp *create_v32(
	struct modem *modem, enum DP_ID id, int caller, int sample_rate,
	int max_fragment, struct dp_operations *operations)
{
	struct sippymodem_v32_datapump *datapump;
	bool is_caller = caller != 0;

	if (modem == NULL || id != DP_V32 || (caller != 0 && caller != 1) ||
	    sample_rate != MODEM_RATE || max_fragment != MODEM_FRAG ||
	    operations != &v32_operations || operations->use_count == INT_MAX ||
	    !rate_is_selected(modem))
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
		    &datapump->startup, endpoint_role(is_caller)) !=
		    SIPPYMODEM_V32_STARTUP_OK) {
		free(datapump);
		return NULL;
	}
	datapump->caller = is_caller;
	datapump->transmit_point = quadrant_point(0);
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

static int next_transmit_point(
	struct sippymodem_v32_datapump *datapump, bool *linked)
{
	uint8_t bits[2] = { 1, 1 };
	uint8_t input = 3;
	uint8_t quadrant;
	enum sippymodem_v32_startup_output output;

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
		if (modem_get_bits(datapump->dp.modem, 1, bits, 2) != 2)
			bits[0] = bits[1] = 1;
		bits[0] &= 1u;
		bits[1] &= 1u;
		input = (uint8_t)((bits[0] << 1) | bits[1]);
	}
	if (sippymodem_v32_symbol_encode(
		    &datapump->encoder, input, &quadrant) !=
	    SIPPYMODEM_V32_FRAMING_OK)
		return -1;
	datapump->transmit_point = quadrant_point(quadrant);
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

static int enter_link(struct sippymodem_v32_datapump *datapump)
{
	if (modem_set_param(
		    datapump->dp.modem, MDMPRM_TX_RATE,
		    SIPPYMODEM_V32_BIT_RATE) != 0 ||
	    modem_set_param(
		    datapump->dp.modem, MDMPRM_RX_RATE,
		    SIPPYMODEM_V32_BIT_RATE) != 0)
		return -1;
	datapump->linked = true;
	datapump->missing_carrier_samples = 0;
	datapump->dp.status = DPSTAT_CONNECT;
	return 0;
}

static int observe_dibit(
	struct sippymodem_v32_datapump *datapump, uint8_t dibit,
	bool *linked)
{
	uint8_t bits[2] = { (uint8_t)(dibit >> 1),
			    (uint8_t)(dibit & 1u) };

	if (datapump->linked) {
		if (datapump->dp.modem->put_bits != NULL)
			(void)modem_put_bits(datapump->dp.modem, 1, bits, 2);
		return 0;
	}
	{
		bool connected;

		if (sippymodem_v32_startup_observe(
			    &datapump->startup, dibit, &connected) !=
		    SIPPYMODEM_V32_STARTUP_OK)
			return -1;
		if (!connected)
			return 0;
		if (enter_link(datapump) != 0)
			return -1;
		*linked = true;
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
		if (sippymodem_v32_symbol_decode(
			    &datapump->decoder, quadrant, &dibit) !=
			    SIPPYMODEM_V32_FRAMING_OK ||
		    observe_dibit(datapump, dibit, linked) != 0)
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
