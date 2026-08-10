// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_fsk.h"

#include <limits.h>
#include <math.h>
#include <string.h>

enum {
	FSK_PROJECTION_SCALE = 16384,
	FSK_MIN_DETECTOR_WINDOW = 3,
	FSK_MAX_SMOOTHING_SHIFT = 30,
};

static const double FSK_TWO_PI = 6.283185307179586476925286766559;
static const double FSK_PHASE_SCALE = 4294967296.0;

static uint64_t symbol_period_units(
	const struct sippymodem_fsk_channel *channel)
{
	return (uint64_t)channel->sample_rate *
	       channel->symbol_rate_denominator;
}

static bool symbol_rate_is_valid(
	const struct sippymodem_fsk_channel *channel)
{
	uint64_t period_units;

	if (channel == NULL || channel->sample_rate == 0 ||
	    channel->symbol_rate_numerator == 0 ||
	    channel->symbol_rate_denominator == 0)
		return false;
	period_units = symbol_period_units(channel);
	return channel->symbol_rate_numerator <= period_units;
}

static bool channel_is_valid(const struct sippymodem_fsk_channel *channel)
{
	uint64_t period_units;
	uint64_t samples_per_symbol_q16;

	if (!symbol_rate_is_valid(channel) ||
	    channel->mark_frequency_hz == 0 ||
	    channel->space_frequency_hz == 0 ||
	    channel->mark_frequency_hz == channel->space_frequency_hz ||
	    (uint64_t)channel->mark_frequency_hz * 2 >= channel->sample_rate ||
	    (uint64_t)channel->space_frequency_hz * 2 >= channel->sample_rate ||
	    channel->detector_window < FSK_MIN_DETECTOR_WINDOW ||
	    channel->detector_window > SIPPYMODEM_FSK_MAX_DETECTOR_WINDOW ||
	    channel->metric_smoothing_shift > FSK_MAX_SMOOTHING_SHIFT)
		return false;
	period_units = symbol_period_units(channel);
	if (period_units > (UINT64_MAX >> 16))
		return false;
	samples_per_symbol_q16 =
		(period_units << 16) / channel->symbol_rate_numerator;
	return samples_per_symbol_q16 != 0 &&
	       samples_per_symbol_q16 <= UINT32_MAX;
}

static uint32_t phase_step(uint32_t sample_rate, uint32_t frequency_hz)
{
	uint64_t numerator =
		(uint64_t)frequency_hz * (UINT64_C(1) << 32);

	return (uint32_t)((numerator + sample_rate / 2) / sample_rate);
}

static bool tone_generator_is_valid(
	const struct sippymodem_fsk_tone_generator *generator)
{
	return generator != NULL && generator->sample_rate != 0 &&
	       generator->mark_phase_step != 0 &&
	       generator->space_phase_step != 0 && generator->amplitude > 0;
}

enum sippymodem_fsk_result sippymodem_fsk_tone_generator_init(
	struct sippymodem_fsk_tone_generator *generator,
	const struct sippymodem_fsk_channel *channel, int16_t amplitude)
{
	if (generator == NULL || !channel_is_valid(channel) || amplitude <= 0)
		return SIPPYMODEM_FSK_INVALID_ARGUMENT;
	*generator = (struct sippymodem_fsk_tone_generator){
		.sample_rate = channel->sample_rate,
		.mark_phase_step = phase_step(
			channel->sample_rate, channel->mark_frequency_hz),
		.space_phase_step = phase_step(
			channel->sample_rate, channel->space_frequency_hz),
		.amplitude = amplitude,
	};
	if (!tone_generator_is_valid(generator)) {
		memset(generator, 0, sizeof(*generator));
		return SIPPYMODEM_FSK_INVALID_ARGUMENT;
	}
	return SIPPYMODEM_FSK_OK;
}

enum sippymodem_fsk_result sippymodem_fsk_tone_generator_sample(
	struct sippymodem_fsk_tone_generator *generator, uint8_t bit,
	int16_t *sample)
{
	uint32_t step;
	double radians;

	if (!tone_generator_is_valid(generator) || sample == NULL)
		return SIPPYMODEM_FSK_INVALID_ARGUMENT;
	step = (bit & 1u) ? generator->mark_phase_step :
			    generator->space_phase_step;
	radians = (double)generator->carrier_phase *
		  (FSK_TWO_PI / FSK_PHASE_SCALE);
	*sample = (int16_t)lround(
		sin(radians) * (double)generator->amplitude);
	generator->carrier_phase += step;
	return SIPPYMODEM_FSK_OK;
}

enum sippymodem_fsk_result sippymodem_fsk_transmitter_init(
	struct sippymodem_fsk_transmitter *transmitter,
	const struct sippymodem_fsk_channel *channel, int16_t amplitude)
{
	struct sippymodem_fsk_tone_generator tones;

	if (transmitter == NULL ||
	    sippymodem_fsk_tone_generator_init(
		    &tones, channel, amplitude) != SIPPYMODEM_FSK_OK)
		return SIPPYMODEM_FSK_INVALID_ARGUMENT;
	*transmitter = (struct sippymodem_fsk_transmitter){
		.channel = *channel,
		.tones = tones,
	};
	return SIPPYMODEM_FSK_OK;
}

bool sippymodem_fsk_transmitter_can_accept_bit(
	const struct sippymodem_fsk_transmitter *transmitter)
{
	return transmitter != NULL &&
	       symbol_rate_is_valid(&transmitter->channel) &&
	       transmitter->symbol_accumulator <
		       symbol_period_units(&transmitter->channel) &&
	       tone_generator_is_valid(&transmitter->tones) &&
	       transmitter->tones.sample_rate ==
		       transmitter->channel.sample_rate &&
	       !transmitter->holding_bit;
}

enum sippymodem_fsk_result sippymodem_fsk_transmitter_sample(
	struct sippymodem_fsk_transmitter *transmitter, uint8_t bit,
	int16_t *sample, bool *bit_consumed)
{
	uint64_t period_units;
	uint64_t units_remaining;

	if (transmitter == NULL || sample == NULL || bit_consumed == NULL ||
	    !symbol_rate_is_valid(&transmitter->channel) ||
	    !tone_generator_is_valid(&transmitter->tones) ||
	    transmitter->tones.sample_rate != transmitter->channel.sample_rate)
		return SIPPYMODEM_FSK_INVALID_ARGUMENT;
	period_units = symbol_period_units(&transmitter->channel);
	if (transmitter->symbol_accumulator >= period_units)
		return SIPPYMODEM_FSK_INVALID_ARGUMENT;
	if (!transmitter->holding_bit) {
		transmitter->held_bit = bit & 1u;
		transmitter->holding_bit = true;
	}
	if (sippymodem_fsk_tone_generator_sample(
		    &transmitter->tones, transmitter->held_bit, sample) !=
	    SIPPYMODEM_FSK_OK)
		return SIPPYMODEM_FSK_INVALID_ARGUMENT;

	units_remaining = period_units - transmitter->symbol_accumulator;
	*bit_consumed = false;
	if (transmitter->channel.symbol_rate_numerator >= units_remaining) {
		transmitter->symbol_accumulator =
			transmitter->channel.symbol_rate_numerator -
			units_remaining;
		transmitter->holding_bit = false;
		*bit_consumed = true;
	} else {
		transmitter->symbol_accumulator +=
			transmitter->channel.symbol_rate_numerator;
	}
	return SIPPYMODEM_FSK_OK;
}

static void build_projection(
	struct sippymodem_fsk_tone_projection *projection,
	const struct sippymodem_fsk_channel *channel,
	uint32_t frequency_hz)
{
	/* Generate each matched filter directly from its defining sinusoid.
	 * Q14 rounding limits every coefficient error to half a unit and avoids
	 * storing waveform or coefficient tables in the source. */
	for (size_t tap = 0; tap < channel->detector_window; ++tap) {
		double phase = -FSK_TWO_PI * (double)frequency_hz *
			       (double)tap / (double)channel->sample_rate;

		projection->in_phase[tap] =
			(int16_t)lround(cos(phase) * FSK_PROJECTION_SCALE);
		projection->quadrature[tap] =
			(int16_t)lround(sin(phase) * FSK_PROJECTION_SCALE);
	}
}

static void reset_stream_state(struct sippymodem_fsk_receiver *receiver)
{
	memset(receiver->history, 0, sizeof(receiver->history));
	receiver->history_index = 0;
	receiver->history_count = 0;
	receiver->mark_power = 0;
	receiver->space_power = 0;
	receiver->line_power = 0;
	receiver->smoothed_metric = 0;
	receiver->sample_countdown_q16 = 0;
	receiver->emitted_bits = 0;
	receiver->last_bit = 1;
	receiver->timing_locked = false;
}

enum sippymodem_fsk_result sippymodem_fsk_receiver_init(
	struct sippymodem_fsk_receiver *receiver,
	const struct sippymodem_fsk_channel *channel)
{
	struct sippymodem_fsk_channel selected;
	uint64_t period_units;
	uint64_t samples_per_symbol_q16;

	if (receiver == NULL || !channel_is_valid(channel))
		return SIPPYMODEM_FSK_INVALID_ARGUMENT;
	selected = *channel;
	period_units = symbol_period_units(&selected);
	samples_per_symbol_q16 =
		(period_units << 16) / selected.symbol_rate_numerator;
	memset(receiver, 0, sizeof(*receiver));
	receiver->channel = selected;
	receiver->samples_per_symbol_q16 =
		(uint32_t)samples_per_symbol_q16;
	build_projection(
		&receiver->mark, &selected, selected.mark_frequency_hz);
	build_projection(
		&receiver->space, &selected, selected.space_frequency_hz);
	reset_stream_state(receiver);
	return SIPPYMODEM_FSK_OK;
}

void sippymodem_fsk_receiver_reset(
	struct sippymodem_fsk_receiver *receiver)
{
	if (receiver == NULL || receiver->samples_per_symbol_q16 == 0 ||
	    receiver->channel.detector_window == 0)
		return;
	reset_stream_state(receiver);
}

static int64_t projection_power(
	const struct sippymodem_fsk_receiver *receiver,
	const struct sippymodem_fsk_tone_projection *projection)
{
	int64_t in_phase = 0;
	int64_t quadrature = 0;
	size_t window = receiver->channel.detector_window;

	for (size_t tap = 0; tap < window; ++tap) {
		size_t index =
			(receiver->history_index + window - 1 - tap) % window;
		int32_t sample = receiver->history[index];

		in_phase += sample * projection->in_phase[tap];
		quadrature += sample * projection->quadrature[tap];
	}
	in_phase /= FSK_PROJECTION_SCALE;
	quadrature /= FSK_PROJECTION_SCALE;
	return in_phase * in_phase + quadrature * quadrature;
}

static void update_detector(
	struct sippymodem_fsk_receiver *receiver, int16_t sample)
{
	uint64_t line_power = 0;
	int64_t metric;
	int64_t smoothing_divisor =
		INT64_C(1) << receiver->channel.metric_smoothing_shift;
	size_t window = receiver->channel.detector_window;

	receiver->history[receiver->history_index] = sample;
	receiver->history_index = (receiver->history_index + 1) % window;
	if (receiver->history_count < window)
		receiver->history_count++;
	if (receiver->history_count < window)
		return;

	receiver->mark_power = projection_power(receiver, &receiver->mark);
	receiver->space_power = projection_power(receiver, &receiver->space);
	for (size_t index = 0; index < window; ++index) {
		int32_t value = receiver->history[index];

		line_power += (uint64_t)(value * value);
	}
	receiver->line_power = line_power;
	metric = receiver->mark_power - receiver->space_power;
	receiver->smoothed_metric +=
		(metric - receiver->smoothed_metric) / smoothing_divisor;
}

enum sippymodem_fsk_result sippymodem_fsk_receiver_detect(
	struct sippymodem_fsk_receiver *receiver, int16_t sample,
	uint8_t *bit, bool *decision_ready)
{
	if (receiver == NULL || bit == NULL || decision_ready == NULL ||
	    receiver->samples_per_symbol_q16 == 0 ||
	    receiver->channel.detector_window == 0 ||
	    receiver->channel.detector_window >
		    SIPPYMODEM_FSK_MAX_DETECTOR_WINDOW ||
	    receiver->channel.metric_smoothing_shift >
		    FSK_MAX_SMOOTHING_SHIFT)
		return SIPPYMODEM_FSK_INVALID_ARGUMENT;
	*bit = 1;
	*decision_ready = false;
	update_detector(receiver, sample);
	if (receiver->history_count < receiver->channel.detector_window)
		return SIPPYMODEM_FSK_OK;
	*bit = receiver->smoothed_metric >= 0 ? 1u : 0u;
	*decision_ready = true;
	return SIPPYMODEM_FSK_OK;
}

enum sippymodem_fsk_result sippymodem_fsk_receiver_sample(
	struct sippymodem_fsk_receiver *receiver, int16_t sample,
	uint8_t *bit, bool *bit_ready)
{
	uint8_t decoded;
	uint8_t previous;
	bool decision_ready;

	if (bit == NULL || bit_ready == NULL)
		return SIPPYMODEM_FSK_INVALID_ARGUMENT;
	*bit_ready = false;
	if (sippymodem_fsk_receiver_detect(
		    receiver, sample, &decoded, &decision_ready) !=
	    SIPPYMODEM_FSK_OK)
		return SIPPYMODEM_FSK_INVALID_ARGUMENT;
	if (!decision_ready)
		return SIPPYMODEM_FSK_OK;

	previous = receiver->last_bit;
	receiver->last_bit = decoded;
	if (!receiver->timing_locked) {
		receiver->timing_locked = true;
		receiver->sample_countdown_q16 =
			receiver->samples_per_symbol_q16 / 2;
		return SIPPYMODEM_FSK_OK;
	}
	if (decoded != previous) {
		receiver->sample_countdown_q16 =
			receiver->samples_per_symbol_q16 / 2;
		return SIPPYMODEM_FSK_OK;
	}
	if (receiver->sample_countdown_q16 > (UINT32_C(1) << 16)) {
		receiver->sample_countdown_q16 -= UINT32_C(1) << 16;
		return SIPPYMODEM_FSK_OK;
	}
	receiver->sample_countdown_q16 +=
		receiver->samples_per_symbol_q16 - (UINT32_C(1) << 16);
	receiver->emitted_bits++;
	*bit = decoded;
	*bit_ready = true;
	return SIPPYMODEM_FSK_OK;
}

struct sippymodem_fsk_measurement sippymodem_fsk_receiver_measure(
	const struct sippymodem_fsk_receiver *receiver)
{
	struct sippymodem_fsk_measurement measurement = { 0 };
	uint64_t total;

	if (receiver == NULL)
		return measurement;
	measurement.mark_power = receiver->mark_power > 0 ?
					 (uint64_t)receiver->mark_power : 0;
	measurement.space_power = receiver->space_power > 0 ?
					  (uint64_t)receiver->space_power : 0;
	measurement.line_power = receiver->line_power;
	measurement.ready = receiver->channel.detector_window != 0 &&
			    receiver->history_count ==
				    receiver->channel.detector_window;
	total = measurement.mark_power + measurement.space_power;
	if (total != 0) {
		uint64_t strongest = measurement.mark_power >
					     measurement.space_power ?
					     measurement.mark_power :
					     measurement.space_power;

		measurement.dominance_q8 = strongest > (UINT64_MAX >> 8) ?
						 UINT32_MAX :
			(uint32_t)((strongest << 8) / total);
	}
	return measurement;
}

const char *sippymodem_fsk_result_name(enum sippymodem_fsk_result result)
{
	switch (result) {
	case SIPPYMODEM_FSK_OK:
		return "ok";
	case SIPPYMODEM_FSK_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown FSK result";
}
