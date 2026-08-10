// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_v34_phase2_dpsk.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>

enum {
	V34_PHASE2_DPSK_PULSE_SPAN_SYMBOLS = 6,
};

static const double V34_PHASE2_DPSK_ROLLOFF = 0.75;
static const double V34_PHASE2_DPSK_TWO_PI =
	6.283185307179586476925286766559;
static const double V34_PHASE2_DPSK_PHASE_SCALE = 4294967296.0;
static const double V34_PHASE2_ANSWERING_CARRIER_GAIN =
	0.8912509381337456; /* -1 dB. */
static const double V34_PHASE2_ANSWERING_GUARD_GAIN =
	0.44668359215096315; /* -7 dB. */

static bool role_is_valid(enum sippymodem_v34_phase2_station_role role)
{
	return role == SIPPYMODEM_V34_PHASE2_CALLING_STATION ||
	       role == SIPPYMODEM_V34_PHASE2_ANSWERING_STATION;
}

static uint32_t local_carrier_hz(
	enum sippymodem_v34_phase2_station_role role)
{
	return role == SIPPYMODEM_V34_PHASE2_CALLING_STATION ?
		       SIPPYMODEM_V34_PHASE2_CALLING_CARRIER_HZ :
		       SIPPYMODEM_V34_PHASE2_ANSWERING_CARRIER_HZ;
}

static uint32_t remote_carrier_hz(
	enum sippymodem_v34_phase2_station_role role)
{
	return role == SIPPYMODEM_V34_PHASE2_CALLING_STATION ?
		       SIPPYMODEM_V34_PHASE2_ANSWERING_CARRIER_HZ :
		       SIPPYMODEM_V34_PHASE2_CALLING_CARRIER_HZ;
}

static struct sippymodem_qam_channel phase2_channel(uint32_t carrier_hz)
{
	return (struct sippymodem_qam_channel){
		.sample_rate = SIPPYMODEM_V34_PHASE2_DPSK_SAMPLE_RATE,
		.symbol_rate_numerator =
			SIPPYMODEM_V34_PHASE2_DPSK_BIT_RATE,
		.symbol_rate_denominator = 1,
		.carrier_frequency_hz = carrier_hz,
		.rolloff = V34_PHASE2_DPSK_ROLLOFF,
		.pulse_span_symbols = V34_PHASE2_DPSK_PULSE_SPAN_SYMBOLS,
	};
}

static uint32_t phase_step(uint32_t frequency_hz)
{
	uint64_t scaled =
		(uint64_t)frequency_hz * (UINT64_C(1) << 32);

	return (uint32_t)(
		(scaled + SIPPYMODEM_V34_PHASE2_DPSK_SAMPLE_RATE / 2u) /
		SIPPYMODEM_V34_PHASE2_DPSK_SAMPLE_RATE);
}

static bool channel_matches(
	const struct sippymodem_qam_channel *channel, uint32_t carrier_hz)
{
	return channel != NULL &&
	       channel->sample_rate ==
		       SIPPYMODEM_V34_PHASE2_DPSK_SAMPLE_RATE &&
	       channel->symbol_rate_numerator ==
		       SIPPYMODEM_V34_PHASE2_DPSK_BIT_RATE &&
	       channel->symbol_rate_denominator == 1 &&
	       channel->carrier_frequency_hz == carrier_hz &&
	       channel->rolloff == V34_PHASE2_DPSK_ROLLOFF &&
	       channel->pulse_span_symbols ==
		       V34_PHASE2_DPSK_PULSE_SPAN_SYMBOLS;
}

static bool transmitter_metadata_is_valid(
	const struct sippymodem_v34_phase2_dpsk_transmitter *transmitter)
{
	if (transmitter == NULL || !role_is_valid(transmitter->local_role) ||
	    !channel_matches(
		    &transmitter->carrier.channel,
		    local_carrier_hz(transmitter->local_role)))
		return false;
	if (transmitter->local_role ==
	    SIPPYMODEM_V34_PHASE2_CALLING_STATION)
		return transmitter->guard_amplitude == 0 &&
		       transmitter->guard_phase_step == 0;
	return transmitter->guard_amplitude > 0 &&
	       transmitter->guard_phase_step ==
		       phase_step(SIPPYMODEM_V34_PHASE2_ANSWERING_GUARD_HZ) &&
	       transmitter->carrier.amplitude <=
		       INT16_MAX - transmitter->guard_amplitude;
}

bool sippymodem_v34_phase2_dpsk_transmitter_is_valid(
	const struct sippymodem_v34_phase2_dpsk_transmitter *transmitter)
{
	return transmitter_metadata_is_valid(transmitter) &&
	       sippymodem_qam_modulator_is_valid(&transmitter->carrier);
}

bool sippymodem_v34_phase2_dpsk_transmitter_can_accept_bit(
	const struct sippymodem_v34_phase2_dpsk_transmitter *transmitter)
{
	return sippymodem_v34_phase2_dpsk_transmitter_is_valid(transmitter) &&
	       !transmitter->initial_point_pending &&
	       sippymodem_qam_modulator_can_accept_symbol(
		       &transmitter->carrier);
}

enum sippymodem_v34_phase2_dpsk_result
sippymodem_v34_phase2_dpsk_transmitter_init(
	struct sippymodem_v34_phase2_dpsk_transmitter *transmitter,
	enum sippymodem_v34_phase2_station_role local_role,
	int16_t nominal_amplitude)
{
	struct sippymodem_v34_phase2_dpsk_transmitter initialized = {
		.local_role = local_role,
		.initial_point_pending = true,
	};
	struct sippymodem_qam_channel channel;
	long carrier_amplitude;
	long guard_amplitude = 0;

	if (transmitter == NULL || !role_is_valid(local_role) ||
	    nominal_amplitude <= 0)
		return SIPPYMODEM_V34_PHASE2_DPSK_INVALID_ARGUMENT;
	carrier_amplitude = nominal_amplitude;
	if (local_role == SIPPYMODEM_V34_PHASE2_ANSWERING_STATION) {
		carrier_amplitude = lround(
			(double)nominal_amplitude *
			V34_PHASE2_ANSWERING_CARRIER_GAIN);
		guard_amplitude = lround(
			(double)nominal_amplitude *
			V34_PHASE2_ANSWERING_GUARD_GAIN);
		initialized.guard_phase_step = phase_step(
			SIPPYMODEM_V34_PHASE2_ANSWERING_GUARD_HZ);
	}
	if (carrier_amplitude <= 0 || guard_amplitude < 0 ||
	    carrier_amplitude + guard_amplitude > INT16_MAX)
		return SIPPYMODEM_V34_PHASE2_DPSK_INVALID_ARGUMENT;
	initialized.guard_amplitude = (int16_t)guard_amplitude;
	channel = phase2_channel(local_carrier_hz(local_role));
	if (sippymodem_qam_modulator_init(
		    &initialized.carrier, &channel,
		    (int16_t)carrier_amplitude) != SIPPYMODEM_QAM_OK ||
	    !sippymodem_v34_phase2_dpsk_transmitter_is_valid(&initialized))
		return SIPPYMODEM_V34_PHASE2_DPSK_INVALID_ARGUMENT;
	*transmitter = initialized;
	return SIPPYMODEM_V34_PHASE2_DPSK_OK;
}

static double phase_radians(uint32_t phase)
{
	return (double)phase *
	       (V34_PHASE2_DPSK_TWO_PI / V34_PHASE2_DPSK_PHASE_SCALE);
}

enum sippymodem_v34_phase2_dpsk_result
sippymodem_v34_phase2_dpsk_transmitter_sample(
	struct sippymodem_v34_phase2_dpsk_transmitter *transmitter,
	uint8_t bit, int16_t *sample, bool *bit_accepted)
{
	struct sippymodem_qam_point point;
	bool can_accept;
	bool inverted;
	bool symbol_accepted;
	bool consumed = false;
	int16_t carrier_sample;
	long mixed;

	if (!sippymodem_v34_phase2_dpsk_transmitter_is_valid(transmitter) ||
	    bit > 1 ||
	    sample == NULL || bit_accepted == NULL)
		return SIPPYMODEM_V34_PHASE2_DPSK_INVALID_ARGUMENT;
	can_accept = sippymodem_qam_modulator_can_accept_symbol(
		&transmitter->carrier);
	inverted = transmitter->point_inverted;
	if (can_accept && !transmitter->initial_point_pending && bit != 0)
		inverted = !inverted;
	point = (struct sippymodem_qam_point){
		.in_phase = inverted ? -1.0 : 1.0,
	};
	if (sippymodem_qam_modulator_sample(
		    &transmitter->carrier, &point, &carrier_sample,
		    &symbol_accepted) != SIPPYMODEM_QAM_OK)
		return SIPPYMODEM_V34_PHASE2_DPSK_INVALID_ARGUMENT;
	if (symbol_accepted) {
		if (transmitter->initial_point_pending) {
			transmitter->initial_point_pending = false;
		} else {
			transmitter->point_inverted = inverted;
			consumed = true;
		}
	}
	mixed = carrier_sample;
	if (transmitter->guard_amplitude != 0) {
		mixed += lround(
			(double)transmitter->guard_amplitude *
			sin(phase_radians(transmitter->guard_phase)));
		transmitter->guard_phase += transmitter->guard_phase_step;
	}
	*sample = (int16_t)mixed;
	*bit_accepted = consumed;
	return SIPPYMODEM_V34_PHASE2_DPSK_OK;
}

static bool point_has_direction(const struct sippymodem_qam_point *point)
{
	return point != NULL && isfinite(point->in_phase) &&
	       isfinite(point->quadrature) &&
	       isfinite(hypot(point->in_phase, point->quadrature)) &&
	       (point->in_phase != 0.0 || point->quadrature != 0.0);
}

static bool receiver_metadata_is_valid(
	const struct sippymodem_v34_phase2_dpsk_receiver *receiver)
{
	if (receiver == NULL || !role_is_valid(receiver->local_role) ||
	    !channel_matches(
		    &receiver->carrier.channel,
		    remote_carrier_hz(receiver->local_role)))
		return false;
	if (receiver->has_previous_point)
		return point_has_direction(&receiver->previous_point);
	return receiver->previous_point.in_phase == 0.0 &&
	       receiver->previous_point.quadrature == 0.0;
}

enum sippymodem_v34_phase2_dpsk_result
sippymodem_v34_phase2_dpsk_receiver_init(
	struct sippymodem_v34_phase2_dpsk_receiver *receiver,
	enum sippymodem_v34_phase2_station_role local_role)
{
	struct sippymodem_v34_phase2_dpsk_receiver initialized = {
		.local_role = local_role,
	};
	struct sippymodem_qam_channel channel;

	if (receiver == NULL || !role_is_valid(local_role))
		return SIPPYMODEM_V34_PHASE2_DPSK_INVALID_ARGUMENT;
	channel = phase2_channel(remote_carrier_hz(local_role));
	if (sippymodem_qam_demodulator_init(
		    &initialized.carrier, &channel) != SIPPYMODEM_QAM_OK ||
	    !receiver_metadata_is_valid(&initialized))
		return SIPPYMODEM_V34_PHASE2_DPSK_INVALID_ARGUMENT;
	*receiver = initialized;
	return SIPPYMODEM_V34_PHASE2_DPSK_OK;
}

enum sippymodem_v34_phase2_dpsk_result
sippymodem_v34_phase2_dpsk_receiver_reset(
	struct sippymodem_v34_phase2_dpsk_receiver *receiver)
{
	if (!receiver_metadata_is_valid(receiver) ||
	    sippymodem_qam_demodulator_reset(&receiver->carrier) !=
		    SIPPYMODEM_QAM_OK)
		return SIPPYMODEM_V34_PHASE2_DPSK_INVALID_ARGUMENT;
	receiver->previous_point = (struct sippymodem_qam_point){ 0 };
	receiver->has_previous_point = false;
	return SIPPYMODEM_V34_PHASE2_DPSK_OK;
}

enum sippymodem_v34_phase2_dpsk_result
sippymodem_v34_phase2_dpsk_receiver_sample(
	struct sippymodem_v34_phase2_dpsk_receiver *receiver,
	int16_t sample, uint8_t *bit, bool *bit_ready)
{
	struct sippymodem_qam_point point;
	bool symbol_ready;
	bool decoded = false;
	uint8_t received = 0;

	if (!receiver_metadata_is_valid(receiver) || bit == NULL ||
	    bit_ready == NULL)
		return SIPPYMODEM_V34_PHASE2_DPSK_INVALID_ARGUMENT;
	if (sippymodem_qam_demodulator_sample(
		    &receiver->carrier, sample, &point, &symbol_ready) !=
	    SIPPYMODEM_QAM_OK)
		return SIPPYMODEM_V34_PHASE2_DPSK_INVALID_ARGUMENT;
	if (symbol_ready) {
		double magnitude;

		if (!point_has_direction(&point))
			return SIPPYMODEM_V34_PHASE2_DPSK_INVALID_ARGUMENT;
		magnitude = hypot(point.in_phase, point.quadrature);
		if (receiver->has_previous_point) {
			double previous_magnitude = hypot(
				receiver->previous_point.in_phase,
				receiver->previous_point.quadrature);
			double dot =
				receiver->previous_point.in_phase /
					previous_magnitude *
					(point.in_phase / magnitude) +
				receiver->previous_point.quadrature /
					previous_magnitude *
					(point.quadrature / magnitude);

			received = dot < 0.0;
			decoded = true;
		}
		receiver->previous_point = point;
		receiver->has_previous_point = true;
	}
	*bit = received;
	*bit_ready = decoded;
	return SIPPYMODEM_V34_PHASE2_DPSK_OK;
}

struct sippymodem_qam_measurement
sippymodem_v34_phase2_dpsk_receiver_measure(
	const struct sippymodem_v34_phase2_dpsk_receiver *receiver)
{
	if (!receiver_metadata_is_valid(receiver))
		return (struct sippymodem_qam_measurement){ 0 };
	return sippymodem_qam_demodulator_measure(&receiver->carrier);
}

const char *sippymodem_v34_phase2_dpsk_result_name(
	enum sippymodem_v34_phase2_dpsk_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_PHASE2_DPSK_OK:
		return "ok";
	case SIPPYMODEM_V34_PHASE2_DPSK_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
