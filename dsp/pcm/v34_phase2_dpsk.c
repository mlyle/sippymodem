// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase2_dpsk.h"

#include "phase.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

static const double V34_PHASE2_DPSK_ROLLOFF = 0.75;
static const double V34_PHASE2_ANSWERING_CARRIER_GAIN =
	0.8912509381337456; /* -1 dB. */
static const double V34_PHASE2_ANSWERING_GUARD_GAIN =
	0.44668359215096315; /* -7 dB. */

/* The role-selected Q16 carrier filter removes the other Phase 2 carrier and
 * the answering guard before differential detection. Both 60-tap filters are
 * symmetric, so only one half is retained. */
static const int16_t v34_phase2_carrier_2400_q16
	[V34_PHASE2_DPSK_CARRIER_FILTER_TAPS / 2] = {
	1, -3, 6, -8, 7, -76, 219, -3, -287, 143,
	63, 97, -2, -603, 520, 620, -1031, -1, 505, 75,
	618, -2116, -1, 4871, -3932, -4825, 9114, -1, -10948, 7012,
};

static const int16_t v34_phase2_carrier_1200_q16
	[V34_PHASE2_DPSK_CARRIER_FILTER_TAPS / 2] = {
	0, 0, -1, 1, 0, -55, -225, -207, 47, 218,
	96, -15, 254, 628, 402, -478, -1072, -653, 82, -114,
	-937, -348, 2549, 5057, 3036, -3727, -9456, -7573, 1801, 10628,
};

/* The INFO receiver works at 24 kHz. These compact Q14 filters retain both
 * Phase 2 carriers while suppressing the interpolation image, then retain the
 * 600-bit/s differential decisions below 360 Hz. The third interpolation
 * phase is the reverse of the first and the low-pass is symmetric. */
static const int16_t v34_phase2_interpolator_q14[2]
	[V34_PHASE2_DPSK_INTERPOLATOR_TAPS] = {
	{
		-133, 336, 192, -2224, 3753, 13940,
		7918, -1709, -658, 586, -90, -58,
	},
	{
		-108, 70, 603, -1655, 313, 11694,
		11694, 313, -1655, 603, 70, -108,
	},
};

static const int16_t v34_phase2_lowpass_q14
	[V34_PHASE2_DPSK_LOWPASS_TAPS / 2] = {
	-19, -17, -15, -13, -10, -6, -1, 6,
	16, 29, 45, 65, 89, 118, 152, 191,
	236, 285, 340, 400, 464, 533, 606, 681,
	759, 839, 919, 999, 1078, 1155, 1229, 1298,
	1363, 1422, 1474, 1518, 1554, 1582, 1601, 1610,
};

static bool role_is_valid(enum v34_station_role role)
{
	return role == V34_CALLING_STATION ||
	       role == V34_ANSWERING_STATION;
}

static uint32_t local_carrier_hz(
	enum v34_station_role role)
{
	return role == V34_CALLING_STATION ?
		       V34_PHASE2_CALLING_CARRIER_HZ :
		       V34_PHASE2_ANSWERING_CARRIER_HZ;
}

static struct qam_channel phase2_channel(uint32_t carrier_hz)
{
	return (struct qam_channel){
		.sample_rate = V34_PHASE2_SAMPLE_RATE,
		.symbol_rate_numerator =
			V34_PHASE2_DPSK_BIT_RATE,
		.symbol_rate_denominator = 1,
		.carrier_frequency_numerator = carrier_hz,
		.carrier_frequency_denominator = 1,
		.rolloff = V34_PHASE2_DPSK_ROLLOFF,
		.pulse_span_symbols =
			V34_PHASE2_DPSK_PULSE_SPAN_SYMBOLS,
	};
}

static bool channel_matches(
	const struct qam_channel *channel, uint32_t carrier_hz)
{
	return channel != NULL &&
	       channel->sample_rate == V34_PHASE2_SAMPLE_RATE &&
	       channel->symbol_rate_numerator ==
		       V34_PHASE2_DPSK_BIT_RATE &&
	       channel->symbol_rate_denominator == 1 &&
	       channel->carrier_frequency_numerator == carrier_hz &&
	       channel->carrier_frequency_denominator == 1 &&
	       channel->rolloff == V34_PHASE2_DPSK_ROLLOFF &&
	       channel->pulse_span_symbols ==
		       V34_PHASE2_DPSK_PULSE_SPAN_SYMBOLS;
}

static bool transmitter_metadata_is_valid(
	const struct v34_phase2_dpsk_transmitter *transmitter)
{
	if (transmitter == NULL || !role_is_valid(transmitter->local_role) ||
	    !channel_matches(
		    &transmitter->carrier.channel,
		    local_carrier_hz(transmitter->local_role)))
		return false;
	if (transmitter->local_role ==
	    V34_CALLING_STATION)
		return transmitter->guard_amplitude == 0 &&
		       transmitter->guard_phase_step == 0;
	return transmitter->guard_amplitude > 0 &&
	       transmitter->guard_phase_step ==
		       phase_step_u32(
			       V34_PHASE2_SAMPLE_RATE,
			       V34_PHASE2_ANSWERING_GUARD_HZ, 1) &&
	       transmitter->carrier.amplitude <=
		       INT16_MAX - transmitter->guard_amplitude;
}

bool v34_phase2_dpsk_transmitter_is_valid(
	const struct v34_phase2_dpsk_transmitter *transmitter)
{
	return transmitter_metadata_is_valid(transmitter) &&
	       qam_modulator_is_valid(&transmitter->carrier);
}

bool v34_phase2_dpsk_transmitter_can_accept_bit(
	const struct v34_phase2_dpsk_transmitter *transmitter)
{
	return v34_phase2_dpsk_transmitter_is_valid(transmitter) &&
	       !transmitter->initial_point_pending &&
	       qam_modulator_can_accept_symbol(
		       &transmitter->carrier);
}

enum v34_phase2_dpsk_result
v34_phase2_dpsk_transmitter_init(
	struct v34_phase2_dpsk_transmitter *transmitter,
	enum v34_station_role local_role,
	int16_t nominal_amplitude)
{
	struct v34_phase2_dpsk_transmitter initialized = {
		.local_role = local_role,
		.initial_point_pending = true,
	};
	struct qam_channel channel;
	long carrier_amplitude;
	long guard_amplitude = 0;

	if (transmitter == NULL || !role_is_valid(local_role) ||
	    nominal_amplitude <= 0)
		return V34_PHASE2_DPSK_INVALID_ARGUMENT;
	carrier_amplitude = nominal_amplitude;
	if (local_role == V34_ANSWERING_STATION) {
		carrier_amplitude = lround(
			(double)nominal_amplitude *
			V34_PHASE2_ANSWERING_CARRIER_GAIN);
		guard_amplitude = lround(
			(double)nominal_amplitude *
			V34_PHASE2_ANSWERING_GUARD_GAIN);
		initialized.guard_phase_step = phase_step_u32(
			V34_PHASE2_SAMPLE_RATE,
			V34_PHASE2_ANSWERING_GUARD_HZ, 1);
	}
	if (carrier_amplitude <= 0 || guard_amplitude < 0 ||
	    carrier_amplitude + guard_amplitude > INT16_MAX)
		return V34_PHASE2_DPSK_INVALID_ARGUMENT;
	initialized.guard_amplitude = (int16_t)guard_amplitude;
	channel = phase2_channel(local_carrier_hz(local_role));
	if (qam_modulator_init(
		    &initialized.carrier, &channel,
		    (int16_t)carrier_amplitude) != QAM_OK ||
	    !v34_phase2_dpsk_transmitter_is_valid(&initialized))
		return V34_PHASE2_DPSK_INVALID_ARGUMENT;
	*transmitter = initialized;
	return V34_PHASE2_DPSK_OK;
}

enum v34_phase2_dpsk_result
v34_phase2_dpsk_transmitter_sample(
	struct v34_phase2_dpsk_transmitter *transmitter,
	uint8_t bit, int16_t *sample, bool *bit_accepted)
{
	struct qam_point point;
	bool can_accept;
	bool inverted;
	bool symbol_accepted;
	bool consumed = false;
	int16_t carrier_sample;
	long mixed;

	if (!v34_phase2_dpsk_transmitter_is_valid(transmitter) ||
	    bit > 1 ||
	    sample == NULL || bit_accepted == NULL)
		return V34_PHASE2_DPSK_INVALID_ARGUMENT;
	can_accept = qam_modulator_can_accept_symbol(
		&transmitter->carrier);
	inverted = transmitter->point_inverted;
	if (can_accept && !transmitter->initial_point_pending && bit != 0)
		inverted = !inverted;
	point = (struct qam_point){
		.in_phase = inverted ? -1.0 : 1.0,
	};
	if (qam_modulator_sample(
		    &transmitter->carrier, &point, &carrier_sample,
		    &symbol_accepted) != QAM_OK)
		return V34_PHASE2_DPSK_INVALID_ARGUMENT;
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
			sin(phase_radians_u32(transmitter->guard_phase)));
		transmitter->guard_phase += transmitter->guard_phase_step;
	}
	*sample = (int16_t)mixed;
	*bit_accepted = consumed;
	return V34_PHASE2_DPSK_OK;
}

bool v34_phase2_dpsk_receiver_is_valid(
	const struct v34_phase2_dpsk_receiver *receiver)
{
	return receiver != NULL && role_is_valid(receiver->local_role) &&
	       receiver->input_count < V34_PHASE2_DPSK_INPUT_BLOCK &&
	       receiver->carrier_index <
		       V34_PHASE2_DPSK_CARRIER_FILTER_TAPS &&
	       receiver->delay_index <
		       V34_PHASE2_DPSK_DIFFERENTIAL_DELAY &&
	       receiver->lowpass_index < V34_PHASE2_DPSK_LOWPASS_TAPS &&
	       receiver->decimation_phase <
		       V34_PHASE2_DPSK_DETECTOR_DECIMATION &&
	       receiver->observations_until_bit <=
		       V34_PHASE2_DPSK_TIMING_OBSERVATIONS &&
	       receiver->timing_locked ==
		       (receiver->observations_until_bit != 0) &&
	       isfinite(receiver->detector_output) &&
	       isfinite(receiver->line_power) && receiver->line_power >= 0.0 &&
	       isfinite(receiver->symbol_power) &&
	       receiver->symbol_power >= 0.0;
}

enum v34_phase2_dpsk_result
v34_phase2_dpsk_receiver_init(
	struct v34_phase2_dpsk_receiver *receiver,
	enum v34_station_role local_role)
{
	struct v34_phase2_dpsk_receiver initialized = {
		.local_role = local_role,
	};

	if (receiver == NULL || !role_is_valid(local_role))
		return V34_PHASE2_DPSK_INVALID_ARGUMENT;
	if (!v34_phase2_dpsk_receiver_is_valid(&initialized))
		return V34_PHASE2_DPSK_INVALID_ARGUMENT;
	*receiver = initialized;
	return V34_PHASE2_DPSK_OK;
}

enum v34_phase2_dpsk_result
v34_phase2_dpsk_receiver_reset(
	struct v34_phase2_dpsk_receiver *receiver)
{
	enum v34_station_role local_role;

	if (!v34_phase2_dpsk_receiver_is_valid(receiver))
		return V34_PHASE2_DPSK_INVALID_ARGUMENT;
	local_role = receiver->local_role;
	return v34_phase2_dpsk_receiver_init(receiver, local_role);
}

static int16_t q14_shift(int64_t accumulator, bool round)
{
	int64_t shifted;

	if (round)
		accumulator += INT64_C(8192);
	if (accumulator >= 0)
		shifted = accumulator / INT64_C(16384);
	else
		shifted = -((-accumulator + INT64_C(16383)) /
			    INT64_C(16384));
	if (shifted > INT16_MAX)
		return INT16_MAX;
	if (shifted < INT16_MIN)
		return INT16_MIN;
	return (int16_t)shifted;
}

static int16_t select_carrier(
	struct v34_phase2_dpsk_receiver *receiver, int16_t sample)
{
	const int16_t *coefficients =
		receiver->local_role == V34_CALLING_STATION ?
			v34_phase2_carrier_2400_q16 :
			v34_phase2_carrier_1200_q16;
	int64_t accumulator = INT64_C(32768);
	size_t next;

	receiver->carrier_history[receiver->carrier_index] = sample;
	next = (receiver->carrier_index + 1u) %
	       V34_PHASE2_DPSK_CARRIER_FILTER_TAPS;
	receiver->carrier_index = next;
	for (size_t tap = 0;
	     tap < V34_PHASE2_DPSK_CARRIER_FILTER_TAPS / 2u; ++tap) {
		size_t recent =
			(next + V34_PHASE2_DPSK_CARRIER_FILTER_TAPS - 1u - tap) %
			V34_PHASE2_DPSK_CARRIER_FILTER_TAPS;
		size_t old =
			(next + tap) % V34_PHASE2_DPSK_CARRIER_FILTER_TAPS;

		accumulator +=
			(int32_t)coefficients[tap] *
			((int32_t)receiver->carrier_history[recent] +
			 receiver->carrier_history[old]);
	}
	if (accumulator >= 0)
		accumulator /= INT64_C(65536);
	else
		accumulator = -((-accumulator + INT64_C(65535)) /
				INT64_C(65536));
	if (accumulator > INT16_MAX)
		return INT16_MAX;
	if (accumulator < INT16_MIN)
		return INT16_MIN;
	return (int16_t)accumulator;
}

static int16_t interpolate(
	const struct v34_phase2_dpsk_receiver *receiver, unsigned int phase)
{
	const int16_t *coefficients =
		v34_phase2_interpolator_q14[phase == 1u];
	bool reverse = phase != 2u;
	int64_t accumulator = 0;

	for (size_t tap = 0;
	     tap < V34_PHASE2_DPSK_INTERPOLATOR_TAPS; ++tap) {
		size_t coefficient = reverse ?
			V34_PHASE2_DPSK_INTERPOLATOR_TAPS - 1u - tap :
			tap;

		accumulator +=
			(int32_t)receiver->interpolation_history[tap] *
			coefficients[coefficient];
	}
	return q14_shift(accumulator, true);
}

static int16_t differential_product(
	struct v34_phase2_dpsk_receiver *receiver, int16_t current)
{
	int16_t delayed = receiver->delay_history[receiver->delay_index];

	receiver->delay_history[receiver->delay_index] = current;
	receiver->delay_index =
		(receiver->delay_index + 1u) %
		V34_PHASE2_DPSK_DIFFERENTIAL_DELAY;
	return q14_shift((int32_t)current * delayed, false);
}

static int16_t lowpass_detector(
	struct v34_phase2_dpsk_receiver *receiver, int16_t current)
{
	int64_t accumulator = 0;
	size_t next;

	receiver->lowpass_history[receiver->lowpass_index] = current;
	next = (receiver->lowpass_index + 1u) %
	       V34_PHASE2_DPSK_LOWPASS_TAPS;
	receiver->lowpass_index = next;
	for (size_t tap = 0; tap < V34_PHASE2_DPSK_LOWPASS_TAPS / 2u;
	     ++tap) {
		size_t recent =
			(next + V34_PHASE2_DPSK_LOWPASS_TAPS - 1u - tap) %
			V34_PHASE2_DPSK_LOWPASS_TAPS;
		size_t old = (next + tap) % V34_PHASE2_DPSK_LOWPASS_TAPS;

		accumulator +=
			(int32_t)v34_phase2_lowpass_q14[tap] *
			((int32_t)receiver->lowpass_history[recent] +
			 receiver->lowpass_history[old]);
	}
	return q14_shift(accumulator, false);
}

static bool crossed_zero(double previous, int16_t current)
{
	return (previous < 0.0 && current >= 0.0) ||
	       (previous >= 0.0 && current < 0.0);
}

static bool observe_detector(
	struct v34_phase2_dpsk_receiver *receiver, int16_t current,
	uint8_t *bit)
{
	bool ready = false;

	receiver->symbol_power +=
		(fabs((double)current) - receiver->symbol_power) / 64.0;
	if (crossed_zero(receiver->detector_output, current)) {
		receiver->timing_locked = true;
		receiver->observations_until_bit =
			V34_PHASE2_DPSK_TIMING_OBSERVATIONS / 2u + 1u;
	} else if (receiver->timing_locked &&
		   --receiver->observations_until_bit == 0) {
		*bit = current <= 0.0;
		ready = true;
		receiver->observations_until_bit =
			V34_PHASE2_DPSK_TIMING_OBSERVATIONS;
	}
	receiver->detector_output = current;
	return ready;
}

static bool process_input_block(
	struct v34_phase2_dpsk_receiver *receiver, uint8_t *bit)
{
	int16_t interpolated[
		V34_PHASE2_DPSK_INPUT_BLOCK * V34_PHASE2_DPSK_INTERPOLATION];
	bool ready = false;

	for (size_t input = 0; input < V34_PHASE2_DPSK_INPUT_BLOCK; ++input) {
		memmove(
			&receiver->interpolation_history[1],
			&receiver->interpolation_history[0],
			(V34_PHASE2_DPSK_INTERPOLATOR_TAPS - 1u) *
				sizeof(receiver->interpolation_history[0]));
		receiver->interpolation_history[0] = receiver->input[input];
		for (unsigned int phase = 0;
		     phase < V34_PHASE2_DPSK_INTERPOLATION; ++phase)
			interpolated[input * V34_PHASE2_DPSK_INTERPOLATION + phase] =
				interpolate(receiver, phase);
	}
	for (size_t index = 0;
	     index < sizeof(interpolated) / sizeof(interpolated[0]); ++index) {
		int16_t product = differential_product(receiver, interpolated[index]);
		int16_t filtered = lowpass_detector(receiver, product);

		if (++receiver->decimation_phase !=
		    V34_PHASE2_DPSK_DETECTOR_DECIMATION)
			continue;
		receiver->decimation_phase = 0;
		if (observe_detector(receiver, filtered, bit))
			ready = true;
	}
	return ready;
}

enum v34_phase2_dpsk_result
v34_phase2_dpsk_receiver_sample(
	struct v34_phase2_dpsk_receiver *receiver,
	int16_t sample, uint8_t *bit, bool *bit_ready)
{
	uint8_t received = 0;
	bool ready = false;

	if (!v34_phase2_dpsk_receiver_is_valid(receiver) ||
	    bit == NULL ||
	    bit_ready == NULL)
		return V34_PHASE2_DPSK_INVALID_ARGUMENT;
	receiver->input[receiver->input_count++] =
		select_carrier(receiver, sample);
	receiver->line_power +=
		((double)sample * sample - receiver->line_power) / 256.0;
	if (receiver->input_count == V34_PHASE2_DPSK_INPUT_BLOCK) {
		receiver->input_count = 0;
		ready = process_input_block(receiver, &received);
	}
	if (!v34_phase2_dpsk_receiver_is_valid(receiver))
		return V34_PHASE2_DPSK_INVALID_ARGUMENT;
	*bit = received;
	*bit_ready = ready;
	return V34_PHASE2_DPSK_OK;
}

struct v34_phase2_dpsk_measurement
v34_phase2_dpsk_receiver_measure(
	const struct v34_phase2_dpsk_receiver *receiver)
{
	if (!v34_phase2_dpsk_receiver_is_valid(receiver))
		return (struct v34_phase2_dpsk_measurement){ 0 };
	return (struct v34_phase2_dpsk_measurement){
		.line_power = receiver->line_power,
		.symbol_power = receiver->symbol_power,
		.timing_locked = receiver->timing_locked,
	};
}
