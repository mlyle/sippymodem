// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_v21.h"

#include <limits.h>
#include <math.h>
#include <string.h>

enum {
	V21_CHANNEL_1_MARK_HZ = 980,
	V21_CHANNEL_1_SPACE_HZ = 1180,
	V21_CHANNEL_2_MARK_HZ = 1650,
	V21_CHANNEL_2_SPACE_HZ = 1850,
	V21_TRANSMIT_AMPLITUDE = 8192,
	V21_PROJECTION_SCALE = 16384,
	V21_METRIC_SMOOTHING = 4,
};

static const double V21_TWO_PI = 6.283185307179586476925286766559;
static const double V21_PHASE_SCALE = 4294967296.0;

static bool channel_frequencies(
	enum sippymodem_v21_channel channel,
	unsigned int *mark_hz,
	unsigned int *space_hz)
{
	/* ITU-T V.21 defines channel 1 as 980/1180 Hz and channel 2 as
	 * 1650/1850 Hz for mark/space respectively. */
	if (mark_hz == NULL || space_hz == NULL)
		return false;
	switch (channel) {
	case SIPPYMODEM_V21_CHANNEL_1:
		*mark_hz = V21_CHANNEL_1_MARK_HZ;
		*space_hz = V21_CHANNEL_1_SPACE_HZ;
		return true;
	case SIPPYMODEM_V21_CHANNEL_2:
		*mark_hz = V21_CHANNEL_2_MARK_HZ;
		*space_hz = V21_CHANNEL_2_SPACE_HZ;
		return true;
	}
	return false;
}

static uint32_t phase_step(unsigned int frequency_hz)
{
	uint64_t numerator =
		(uint64_t)frequency_hz * (UINT64_C(1) << 32);

	return (uint32_t)((numerator + SIPPYMODEM_V21_SAMPLE_RATE / 2) /
			  SIPPYMODEM_V21_SAMPLE_RATE);
}

enum sippymodem_v21_result sippymodem_v21_transmitter_init(
	struct sippymodem_v21_transmitter *transmitter,
	enum sippymodem_v21_channel channel)
{
	unsigned int mark_hz;
	unsigned int space_hz;

	if (transmitter == NULL ||
	    !channel_frequencies(channel, &mark_hz, &space_hz))
		return SIPPYMODEM_V21_INVALID_ARGUMENT;
	*transmitter = (struct sippymodem_v21_transmitter){
		.channel = channel,
		.mark_phase_step = phase_step(mark_hz),
		.space_phase_step = phase_step(space_hz),
	};
	return SIPPYMODEM_V21_OK;
}

enum sippymodem_v21_result sippymodem_v21_transmitter_sample(
	struct sippymodem_v21_transmitter *transmitter, uint8_t bit,
	int16_t *sample, bool *bit_consumed)
{
	uint32_t step;
	double radians;

	if (transmitter == NULL || sample == NULL || bit_consumed == NULL ||
	    transmitter->mark_phase_step == 0 ||
	    transmitter->space_phase_step == 0 ||
	    (transmitter->channel != SIPPYMODEM_V21_CHANNEL_1 &&
	     transmitter->channel != SIPPYMODEM_V21_CHANNEL_2))
		return SIPPYMODEM_V21_INVALID_ARGUMENT;
	if (!transmitter->holding_bit) {
		transmitter->held_bit = bit & 1u;
		transmitter->holding_bit = true;
	}
	step = transmitter->held_bit ? transmitter->mark_phase_step :
					      transmitter->space_phase_step;
	radians = (double)transmitter->carrier_phase *
		  (V21_TWO_PI / V21_PHASE_SCALE);
	*sample = (int16_t)lround(
		sin(radians) * (double)V21_TRANSMIT_AMPLITUDE);
	transmitter->carrier_phase += step;

	transmitter->baud_accumulator += SIPPYMODEM_V21_BAUD;
	*bit_consumed = false;
	if (transmitter->baud_accumulator >= SIPPYMODEM_V21_SAMPLE_RATE) {
		transmitter->baud_accumulator -= SIPPYMODEM_V21_SAMPLE_RATE;
		transmitter->holding_bit = false;
		*bit_consumed = true;
	}
	return SIPPYMODEM_V21_OK;
}

static void build_projection(
	struct sippymodem_v21_tone_projection *projection,
	unsigned int frequency_hz)
{
	/* Generate the one-symbol matched filters directly from their defining
	 * sinusoids. Q14 rounding limits each coefficient error to half a unit
	 * and avoids carrying a waveform or coefficient table in the source. */
	for (size_t tap = 0; tap < SIPPYMODEM_V21_DETECTOR_WINDOW; ++tap) {
		double phase = -V21_TWO_PI * (double)frequency_hz *
			       (double)tap /
			       (double)SIPPYMODEM_V21_SAMPLE_RATE;

		projection->in_phase[tap] =
			(int16_t)lround(cos(phase) * V21_PROJECTION_SCALE);
		projection->quadrature[tap] =
			(int16_t)lround(sin(phase) * V21_PROJECTION_SCALE);
	}
}

static void reset_stream_state(struct sippymodem_v21_receiver *receiver)
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

enum sippymodem_v21_result sippymodem_v21_receiver_init(
	struct sippymodem_v21_receiver *receiver,
	enum sippymodem_v21_channel channel)
{
	unsigned int mark_hz;
	unsigned int space_hz;

	if (receiver == NULL ||
	    !channel_frequencies(channel, &mark_hz, &space_hz))
		return SIPPYMODEM_V21_INVALID_ARGUMENT;
	memset(receiver, 0, sizeof(*receiver));
	receiver->channel = channel;
	receiver->samples_per_bit_q16 = (uint32_t)(
		((uint64_t)SIPPYMODEM_V21_SAMPLE_RATE << 16) /
		SIPPYMODEM_V21_BAUD);
	build_projection(&receiver->mark, mark_hz);
	build_projection(&receiver->space, space_hz);
	reset_stream_state(receiver);
	return SIPPYMODEM_V21_OK;
}

void sippymodem_v21_receiver_reset(
	struct sippymodem_v21_receiver *receiver)
{
	if (receiver == NULL || receiver->samples_per_bit_q16 == 0)
		return;
	reset_stream_state(receiver);
}

static int64_t projection_power(
	const struct sippymodem_v21_receiver *receiver,
	const struct sippymodem_v21_tone_projection *projection)
{
	int64_t in_phase = 0;
	int64_t quadrature = 0;

	for (size_t tap = 0; tap < SIPPYMODEM_V21_DETECTOR_WINDOW; ++tap) {
		size_t index = (receiver->history_index +
				SIPPYMODEM_V21_DETECTOR_WINDOW - 1 - tap) %
			       SIPPYMODEM_V21_DETECTOR_WINDOW;
		int32_t sample = receiver->history[index];

		in_phase += sample * projection->in_phase[tap];
		quadrature += sample * projection->quadrature[tap];
	}
	in_phase /= V21_PROJECTION_SCALE;
	quadrature /= V21_PROJECTION_SCALE;
	return in_phase * in_phase + quadrature * quadrature;
}

static void update_detector(
	struct sippymodem_v21_receiver *receiver, int16_t sample)
{
	uint64_t line_power = 0;
	int64_t metric;

	receiver->history[receiver->history_index] = sample;
	receiver->history_index = (receiver->history_index + 1) %
				  SIPPYMODEM_V21_DETECTOR_WINDOW;
	if (receiver->history_count < SIPPYMODEM_V21_DETECTOR_WINDOW)
		receiver->history_count++;
	if (receiver->history_count < SIPPYMODEM_V21_DETECTOR_WINDOW)
		return;

	receiver->mark_power = projection_power(receiver, &receiver->mark);
	receiver->space_power = projection_power(receiver, &receiver->space);
	for (size_t index = 0; index < SIPPYMODEM_V21_DETECTOR_WINDOW;
	     ++index) {
		int32_t value = receiver->history[index];

		line_power += (uint64_t)(value * value);
	}
	receiver->line_power = line_power;
	metric = receiver->mark_power - receiver->space_power;
	receiver->smoothed_metric +=
		(metric - receiver->smoothed_metric) / V21_METRIC_SMOOTHING;
}

enum sippymodem_v21_result sippymodem_v21_receiver_sample(
	struct sippymodem_v21_receiver *receiver, int16_t sample,
	uint8_t *bit, bool *bit_ready)
{
	uint8_t decoded;
	uint8_t previous;

	if (receiver == NULL || bit == NULL || bit_ready == NULL ||
	    receiver->samples_per_bit_q16 == 0)
		return SIPPYMODEM_V21_INVALID_ARGUMENT;
	*bit_ready = false;
	update_detector(receiver, sample);
	if (receiver->history_count < SIPPYMODEM_V21_DETECTOR_WINDOW)
		return SIPPYMODEM_V21_OK;

	previous = receiver->last_bit;
	decoded = receiver->smoothed_metric >= 0 ? 1u : 0u;
	receiver->last_bit = decoded;
	if (!receiver->timing_locked) {
		receiver->timing_locked = true;
		receiver->sample_countdown_q16 =
			receiver->samples_per_bit_q16 / 2;
		return SIPPYMODEM_V21_OK;
	}
	if (decoded != previous) {
		receiver->sample_countdown_q16 =
			receiver->samples_per_bit_q16 / 2;
		return SIPPYMODEM_V21_OK;
	}
	if (receiver->sample_countdown_q16 > (UINT32_C(1) << 16)) {
		receiver->sample_countdown_q16 -= UINT32_C(1) << 16;
		return SIPPYMODEM_V21_OK;
	}
	receiver->sample_countdown_q16 +=
		receiver->samples_per_bit_q16 - (UINT32_C(1) << 16);
	receiver->emitted_bits++;
	*bit = decoded;
	*bit_ready = true;
	return SIPPYMODEM_V21_OK;
}

struct sippymodem_v21_measurement sippymodem_v21_receiver_measure(
	const struct sippymodem_v21_receiver *receiver)
{
	struct sippymodem_v21_measurement measurement = { 0 };
	uint64_t total;

	if (receiver == NULL)
		return measurement;
	measurement.mark_power = receiver->mark_power > 0 ?
					 (uint64_t)receiver->mark_power : 0;
	measurement.space_power = receiver->space_power > 0 ?
					  (uint64_t)receiver->space_power : 0;
	measurement.line_power = receiver->line_power;
	measurement.ready =
		receiver->history_count == SIPPYMODEM_V21_DETECTOR_WINDOW;
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

const char *sippymodem_v21_result_name(
	enum sippymodem_v21_result result)
{
	switch (result) {
	case SIPPYMODEM_V21_OK:
		return "ok";
	case SIPPYMODEM_V21_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown V.21 result";
}
