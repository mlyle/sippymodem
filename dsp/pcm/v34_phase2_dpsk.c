// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase2_dpsk.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>

static const double V34_PHASE2_DPSK_ROLLOFF = 0.75;
static const double V34_PHASE2_DPSK_TWO_PI =
	6.283185307179586476925286766559;
static const double V34_PHASE2_DPSK_PHASE_SCALE = 4294967296.0;
static const double V34_PHASE2_ANSWERING_CARRIER_GAIN =
	0.8912509381337456; /* -1 dB. */
static const double V34_PHASE2_ANSWERING_GUARD_GAIN =
	0.44668359215096315; /* -7 dB. */
static const double V34_PHASE2_DPSK_DETECTOR_CUTOFF_HZ = 360.0;
static const double V34_PHASE2_DPSK_DETECTOR_Q =
	0.707106781186547524400844362105;
static const double V34_PHASE2_DPSK_GUARD_NOTCH_RADIUS = 0.95;

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

static struct sippymodem_qam_channel phase2_channel(uint32_t carrier_hz)
{
	return (struct sippymodem_qam_channel){
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

static uint32_t phase_step(uint32_t frequency_hz)
{
	uint64_t scaled =
		(uint64_t)frequency_hz * (UINT64_C(1) << 32);

	return (uint32_t)(
		(scaled + V34_PHASE2_SAMPLE_RATE / 2u) /
		V34_PHASE2_SAMPLE_RATE);
}

static bool channel_matches(
	const struct sippymodem_qam_channel *channel, uint32_t carrier_hz)
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
		       phase_step(V34_PHASE2_ANSWERING_GUARD_HZ) &&
	       transmitter->carrier.amplitude <=
		       INT16_MAX - transmitter->guard_amplitude;
}

bool v34_phase2_dpsk_transmitter_is_valid(
	const struct v34_phase2_dpsk_transmitter *transmitter)
{
	return transmitter_metadata_is_valid(transmitter) &&
	       sippymodem_qam_modulator_is_valid(&transmitter->carrier);
}

bool v34_phase2_dpsk_transmitter_can_accept_bit(
	const struct v34_phase2_dpsk_transmitter *transmitter)
{
	return v34_phase2_dpsk_transmitter_is_valid(transmitter) &&
	       !transmitter->initial_point_pending &&
	       sippymodem_qam_modulator_can_accept_symbol(
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
	struct sippymodem_qam_channel channel;
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
		initialized.guard_phase_step = phase_step(
			V34_PHASE2_ANSWERING_GUARD_HZ);
	}
	if (carrier_amplitude <= 0 || guard_amplitude < 0 ||
	    carrier_amplitude + guard_amplitude > INT16_MAX)
		return V34_PHASE2_DPSK_INVALID_ARGUMENT;
	initialized.guard_amplitude = (int16_t)guard_amplitude;
	channel = phase2_channel(local_carrier_hz(local_role));
	if (sippymodem_qam_modulator_init(
		    &initialized.carrier, &channel,
		    (int16_t)carrier_amplitude) != SIPPYMODEM_QAM_OK ||
	    !v34_phase2_dpsk_transmitter_is_valid(&initialized))
		return V34_PHASE2_DPSK_INVALID_ARGUMENT;
	*transmitter = initialized;
	return V34_PHASE2_DPSK_OK;
}

static double phase_radians(uint32_t phase)
{
	return (double)phase *
	       (V34_PHASE2_DPSK_TWO_PI / V34_PHASE2_DPSK_PHASE_SCALE);
}

enum v34_phase2_dpsk_result
v34_phase2_dpsk_transmitter_sample(
	struct v34_phase2_dpsk_transmitter *transmitter,
	uint8_t bit, int16_t *sample, bool *bit_accepted)
{
	struct sippymodem_qam_point point;
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
			sin(phase_radians(transmitter->guard_phase)));
		transmitter->guard_phase += transmitter->guard_phase_step;
	}
	*sample = (int16_t)mixed;
	*bit_accepted = consumed;
	return V34_PHASE2_DPSK_OK;
}

static void configure_guard_filter(
	struct v34_phase2_dpsk_filter *filter)
{
	double radians = V34_PHASE2_DPSK_TWO_PI *
			 V34_PHASE2_ANSWERING_GUARD_HZ /
			 V34_PHASE2_SAMPLE_RATE;
	double cosine = cos(radians);
	double radius = V34_PHASE2_DPSK_GUARD_NOTCH_RADIUS;
	double numerator_at_dc = 2.0 - 2.0 * cosine;
	double denominator_at_dc =
		1.0 - 2.0 * radius * cosine + radius * radius;
	double scale = denominator_at_dc / numerator_at_dc;

	filter->b0 = scale;
	filter->b1 = -2.0 * cosine * scale;
	filter->b2 = scale;
	filter->a1 = -2.0 * radius * cosine;
	filter->a2 = radius * radius;
}

static void configure_lowpass_filter(
	struct v34_phase2_dpsk_filter *filter)
{
	double sample_rate =
		V34_PHASE2_SAMPLE_RATE *
		V34_PHASE2_DPSK_INTERPOLATION;
	double radians = V34_PHASE2_DPSK_TWO_PI *
			 V34_PHASE2_DPSK_DETECTOR_CUTOFF_HZ / sample_rate;
	double cosine = cos(radians);
	double alpha = sin(radians) / (2.0 * V34_PHASE2_DPSK_DETECTOR_Q);
	double scale = 1.0 / (1.0 + alpha);

	filter->b0 = (1.0 - cosine) * 0.5 * scale;
	filter->b1 = (1.0 - cosine) * scale;
	filter->b2 = filter->b0;
	filter->a1 = -2.0 * cosine * scale;
	filter->a2 = (1.0 - alpha) * scale;
}

static void configure_detector_filter(
	struct v34_phase2_dpsk_receiver *receiver)
{
	/* Generate both small filters instead of retaining coefficient tables.
	 * The input notch removes the answering station's 1800 Hz guard before
	 * differential multiplication can mix it with the 2400 Hz carrier. The
	 * 360 Hz Butterworth section then retains the 600-bit/s decisions while
	 * rejecting carrier and interpolation products. */
	configure_guard_filter(&receiver->guard_filter);
	configure_lowpass_filter(&receiver->detector_filter);
}

static bool filter_state_is_finite(
	const struct v34_phase2_dpsk_filter *filter)
{
	return isfinite(filter->input_1) && isfinite(filter->input_2) &&
	       isfinite(filter->output_1) && isfinite(filter->output_2);
}

static bool guard_filter_is_valid(
	const struct v34_phase2_dpsk_filter *filter)
{
	double dc_numerator;
	double dc_denominator;

	if (!filter_state_is_finite(filter) || !isfinite(filter->b0) ||
	    filter->b0 <= 0.0 || filter->b2 != filter->b0 ||
	    !isfinite(filter->b1) || filter->b1 >= 0.0 ||
	    !isfinite(filter->a1) || filter->a1 <= -2.0 ||
	    filter->a1 >= 2.0 || !isfinite(filter->a2) ||
	    filter->a2 <= 0.0 || filter->a2 >= 1.0)
		return false;
	dc_numerator = filter->b0 + filter->b1 + filter->b2;
	dc_denominator = 1.0 + filter->a1 + filter->a2;
	return fabs(dc_numerator - dc_denominator) < 1e-12;
}

static bool lowpass_filter_is_valid(
	const struct v34_phase2_dpsk_filter *filter)
{
	double dc_numerator;
	double dc_denominator;

	if (!filter_state_is_finite(filter) || !isfinite(filter->b0) ||
	    filter->b0 <= 0.0 ||
	    filter->b0 >= 1.0 || filter->b1 != 2.0 * filter->b0 ||
	    filter->b2 != filter->b0 || !isfinite(filter->a1) ||
	    filter->a1 <= -2.0 || filter->a1 >= 0.0 ||
	    !isfinite(filter->a2) || filter->a2 <= 0.0 ||
	    filter->a2 >= 1.0)
		return false;
	dc_numerator = filter->b0 + filter->b1 + filter->b2;
	dc_denominator = 1.0 + filter->a1 + filter->a2;
	return fabs(dc_numerator - dc_denominator) < 1e-12;
}

static bool detector_filter_is_valid(
	const struct v34_phase2_dpsk_receiver *receiver)
{
	return guard_filter_is_valid(&receiver->guard_filter) &&
	       lowpass_filter_is_valid(&receiver->detector_filter);
}

static bool delay_history_is_finite(
	const struct v34_phase2_dpsk_receiver *receiver)
{
	for (size_t index = 0;
	     index < V34_PHASE2_DPSK_DIFFERENTIAL_DELAY; ++index)
		if (!isfinite(receiver->delay_history[index]))
			return false;
	return true;
}

bool v34_phase2_dpsk_receiver_is_valid(
	const struct v34_phase2_dpsk_receiver *receiver)
{
	return receiver != NULL && role_is_valid(receiver->local_role) &&
	       detector_filter_is_valid(receiver) &&
	       delay_history_is_finite(receiver) &&
	       receiver->delay_index <
		       V34_PHASE2_DPSK_DIFFERENTIAL_DELAY &&
	       receiver->delay_count <=
		       V34_PHASE2_DPSK_DIFFERENTIAL_DELAY &&
	       receiver->decimation_phase <
		       V34_PHASE2_DPSK_DETECTOR_DECIMATION &&
	       receiver->observations_until_bit <=
		       V34_PHASE2_DPSK_TIMING_OBSERVATIONS &&
	       receiver->timing_locked ==
		       (receiver->observations_until_bit != 0) &&
	       isfinite(receiver->previous_sample) &&
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
	configure_detector_filter(&initialized);
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

static double interpolate_sample(
	double previous, double current, unsigned int phase)
{
	double fraction = (double)(phase + 1u) /
			  (double)V34_PHASE2_DPSK_INTERPOLATION;

	return previous + (current - previous) * fraction;
}

static double differential_product(
	struct v34_phase2_dpsk_receiver *receiver,
	double current)
{
	double delayed = receiver->delay_history[receiver->delay_index];

	/* Threefold interpolation makes one 600-bit/s interval exactly forty
	 * samples. Multiplication by that delayed waveform removes either INFO
	 * carrier without estimating its phase; a reversal produces a negative
	 * decision and an unchanged phase produces a positive one. */
	receiver->delay_history[receiver->delay_index] = current;
	receiver->delay_index =
		(receiver->delay_index + 1u) %
		V34_PHASE2_DPSK_DIFFERENTIAL_DELAY;
	if (receiver->delay_count <
	    V34_PHASE2_DPSK_DIFFERENTIAL_DELAY) {
		receiver->delay_count++;
		return 0.0;
	}
	return current * delayed;
}

static double filter_detector_section(
	struct v34_phase2_dpsk_filter *filter, double input)
{
	double output = filter->b0 * input + filter->b1 * filter->input_1 +
			filter->b2 * filter->input_2 -
			filter->a1 * filter->output_1 -
			filter->a2 * filter->output_2;

	filter->input_2 = filter->input_1;
	filter->input_1 = input;
	filter->output_2 = filter->output_1;
	filter->output_1 = output;
	return output;
}

static double filter_detector(
	struct v34_phase2_dpsk_receiver *receiver,
	double input)
{
	return filter_detector_section(
		&receiver->detector_filter, input);
}

static bool crossed_zero(double previous, double current)
{
	return (previous < 0.0 && current >= 0.0) ||
	       (previous >= 0.0 && current < 0.0);
}

static bool observe_detector(
	struct v34_phase2_dpsk_receiver *receiver,
	double current, uint8_t *bit)
{
	bool ready = false;

	receiver->symbol_power +=
		(fabs(current) - receiver->symbol_power) / 64.0;
	if (crossed_zero(receiver->detector_output, current)) {
		receiver->timing_locked = true;
		receiver->observations_until_bit =
			V34_PHASE2_DPSK_TIMING_OBSERVATIONS / 2u;
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

enum v34_phase2_dpsk_result
v34_phase2_dpsk_receiver_sample(
	struct v34_phase2_dpsk_receiver *receiver,
	int16_t sample, uint8_t *bit, bool *bit_ready)
{
	struct v34_phase2_dpsk_receiver next;
	double current;
	double previous;
	uint8_t received = 0;
	bool ready = false;

	if (!v34_phase2_dpsk_receiver_is_valid(receiver) ||
	    bit == NULL ||
	    bit_ready == NULL)
		return V34_PHASE2_DPSK_INVALID_ARGUMENT;
	next = *receiver;
	current = sample;
	if (next.local_role == V34_CALLING_STATION)
		current = filter_detector_section(&next.guard_filter, current);
	previous = next.has_previous_sample ? next.previous_sample : current;
	for (unsigned int phase = 0;
	     phase < V34_PHASE2_DPSK_INTERPOLATION; ++phase) {
		double interpolated = interpolate_sample(previous, current, phase);
		double product = differential_product(&next, interpolated);
		double filtered = filter_detector(&next, product);

		if (++next.decimation_phase ==
		    V34_PHASE2_DPSK_DETECTOR_DECIMATION) {
			next.decimation_phase = 0;
			if (observe_detector(&next, filtered, &received))
				ready = true;
		}
	}
	next.previous_sample = current;
	next.has_previous_sample = true;
	next.line_power += ((double)sample * sample - next.line_power) / 256.0;
	if (!v34_phase2_dpsk_receiver_is_valid(&next))
		return V34_PHASE2_DPSK_INVALID_ARGUMENT;
	*receiver = next;
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

const char *v34_phase2_dpsk_result_name(
	enum v34_phase2_dpsk_result result)
{
	switch (result) {
	case V34_PHASE2_DPSK_OK:
		return "ok";
	case V34_PHASE2_DPSK_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
