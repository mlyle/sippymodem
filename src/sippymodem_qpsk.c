// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_qpsk.h"

#include <limits.h>
#include <math.h>
#include <string.h>

enum {
	QPSK_NORMALIZATION_PHASES = 512,
	QPSK_QUADRANT_COUNT = 4,
};

static const double QPSK_PI = 3.1415926535897932384626433832795;
static const double QPSK_TWO_PI = 6.283185307179586476925286766559;
static const double QPSK_PHASE_SCALE = 4294967296.0;
static const double QPSK_SINGULARITY_EPSILON = 1e-10;

static bool channel_is_valid(
	const struct sippymodem_qpsk_channel *channel,
	uint64_t *symbol_period_units)
{
	uint64_t period;

	if (channel == NULL || channel->sample_rate == 0 ||
	    channel->symbol_rate_numerator == 0 ||
	    channel->symbol_rate_denominator == 0 ||
	    channel->carrier_frequency_hz == 0 ||
	    (uint64_t)channel->carrier_frequency_hz * 2 >=
		    channel->sample_rate ||
	    !isfinite(channel->rolloff) || channel->rolloff <= 0.0 ||
	    channel->rolloff > 1.0 || channel->pulse_span_symbols < 2 ||
	    channel->pulse_span_symbols >
		    SIPPYMODEM_QPSK_MAX_PULSE_SPAN_SYMBOLS ||
	    (channel->pulse_span_symbols & 1u) != 0)
		return false;
	period = (uint64_t)channel->sample_rate *
		 channel->symbol_rate_denominator;
	if (period == 0 || channel->symbol_rate_numerator > period ||
	    period > (UINT64_MAX - channel->symbol_rate_numerator) /
			     channel->pulse_span_symbols)
		return false;
	*symbol_period_units = period;
	return true;
}

enum sippymodem_qpsk_result sippymodem_root_raised_cosine(
	double symbol_time, double rolloff, double *value)
{
	double four_rolloff_time;
	double numerator;
	double denominator;

	if (!isfinite(symbol_time) || !isfinite(rolloff) || rolloff <= 0.0 ||
	    rolloff > 1.0 || value == NULL)
		return SIPPYMODEM_QPSK_INVALID_ARGUMENT;
	if (fabs(symbol_time) < QPSK_SINGULARITY_EPSILON) {
		*value = 1.0 + rolloff * (4.0 / QPSK_PI - 1.0);
		return SIPPYMODEM_QPSK_OK;
	}
	four_rolloff_time = 4.0 * rolloff * symbol_time;
	if (fabs(fabs(four_rolloff_time) - 1.0) <
	    QPSK_SINGULARITY_EPSILON) {
		double angle = QPSK_PI / (4.0 * rolloff);

		*value = rolloff / sqrt(2.0) *
			 ((1.0 + 2.0 / QPSK_PI) * sin(angle) +
			  (1.0 - 2.0 / QPSK_PI) * cos(angle));
		return SIPPYMODEM_QPSK_OK;
	}
	numerator = sin(QPSK_PI * symbol_time * (1.0 - rolloff)) +
		    four_rolloff_time *
			    cos(QPSK_PI * symbol_time * (1.0 + rolloff));
	denominator = QPSK_PI * symbol_time *
		      (1.0 - four_rolloff_time * four_rolloff_time);
	*value = numerator / denominator;
	return isfinite(*value) ? SIPPYMODEM_QPSK_OK :
				 SIPPYMODEM_QPSK_INVALID_ARGUMENT;
}

static double pulse_normalization(
	const struct sippymodem_qpsk_channel *channel)
{
	double maximum = 0.0;
	double delay = (double)channel->pulse_span_symbols / 2.0;

	/* Generate a conservative peak bound instead of storing polyphase
	 * coefficients. The final clamp covers the space between scan points. */
	for (unsigned int phase = 0; phase < QPSK_NORMALIZATION_PHASES;
	     ++phase) {
		double fractional =
			(double)phase / (double)QPSK_NORMALIZATION_PHASES;
		double sum = 0.0;

		for (unsigned int age = 0;
		     age <= channel->pulse_span_symbols; ++age) {
			double pulse = 0.0;

			(void)sippymodem_root_raised_cosine(
				(double)age + fractional - delay,
				channel->rolloff, &pulse);
			sum += fabs(pulse);
		}
		if (sum > maximum)
			maximum = sum;
	}
	return maximum * 1.001;
}

static uint32_t phase_step(uint32_t sample_rate, uint32_t frequency_hz)
{
	uint64_t numerator =
		(uint64_t)frequency_hz * (UINT64_C(1) << 32);

	return (uint32_t)((numerator + sample_rate / 2) / sample_rate);
}

static bool modulator_is_valid(
	const struct sippymodem_qpsk_modulator *modulator)
{
	uint64_t period = 0;
	uint64_t support;

	if (modulator == NULL ||
	    !channel_is_valid(&modulator->channel, &period) ||
	    modulator->symbol_period_units != period ||
	    modulator->symbol_phase_units >= period ||
	    modulator->carrier_phase_step == 0 || modulator->amplitude <= 0 ||
	    !isfinite(modulator->pulse_normalization) ||
	    modulator->pulse_normalization <= 0.0 ||
	    modulator->active_count > SIPPYMODEM_QPSK_MAX_ACTIVE_SYMBOLS)
		return false;
	support = period * modulator->channel.pulse_span_symbols;
	if (modulator->active_count == 0 && !modulator->needs_symbol)
		return false;
	for (size_t index = 0; index < modulator->active_count; ++index)
		if (modulator->active[index].age_units > support ||
		    modulator->active[index].in_phase *
				    modulator->active[index].in_phase +
			    modulator->active[index].quadrature *
				    modulator->active[index].quadrature !=
			    1)
			return false;
	return true;
}

enum sippymodem_qpsk_result sippymodem_qpsk_modulator_init(
	struct sippymodem_qpsk_modulator *modulator,
	const struct sippymodem_qpsk_channel *channel, int16_t amplitude)
{
	uint64_t period = 0;
	double normalization;

	if (modulator == NULL || amplitude <= 0 ||
	    !channel_is_valid(channel, &period))
		return SIPPYMODEM_QPSK_INVALID_ARGUMENT;
	normalization = pulse_normalization(channel);
	if (!isfinite(normalization) || normalization <= 0.0)
		return SIPPYMODEM_QPSK_INVALID_ARGUMENT;
	memset(modulator, 0, sizeof(*modulator));
	modulator->channel = *channel;
	modulator->symbol_period_units = period;
	modulator->carrier_phase_step = phase_step(
		channel->sample_rate, channel->carrier_frequency_hz);
	modulator->pulse_normalization = normalization;
	modulator->amplitude = amplitude;
	modulator->needs_symbol = true;
	return SIPPYMODEM_QPSK_OK;
}

bool sippymodem_qpsk_modulator_can_accept_symbol(
	const struct sippymodem_qpsk_modulator *modulator)
{
	return modulator_is_valid(modulator) && modulator->needs_symbol &&
	       modulator->active_count < SIPPYMODEM_QPSK_MAX_ACTIVE_SYMBOLS;
}

static void append_symbol(
	struct sippymodem_qpsk_modulator *modulator, uint8_t quadrant)
{
	static const int8_t in_phase[QPSK_QUADRANT_COUNT] = { 1, 0, -1, 0 };
	static const int8_t quadrature[QPSK_QUADRANT_COUNT] = { 0, 1, 0, -1 };
	struct sippymodem_qpsk_active_symbol *symbol =
		&modulator->active[modulator->active_count++];

	*symbol = (struct sippymodem_qpsk_active_symbol){
		.age_units = modulator->symbol_phase_units,
		.in_phase = in_phase[quadrant],
		.quadrature = quadrature[quadrant],
	};
	modulator->needs_symbol = false;
}

static void baseband_sample(
	const struct sippymodem_qpsk_modulator *modulator,
	double *in_phase, double *quadrature)
{
	double delay = (double)modulator->channel.pulse_span_symbols / 2.0;

	*in_phase = 0.0;
	*quadrature = 0.0;
	for (size_t index = 0; index < modulator->active_count; ++index) {
		const struct sippymodem_qpsk_active_symbol *symbol =
			&modulator->active[index];
		double age = (double)symbol->age_units /
			     (double)modulator->symbol_period_units;
		double pulse = 0.0;

		(void)sippymodem_root_raised_cosine(
			age - delay, modulator->channel.rolloff, &pulse);
		*in_phase += pulse * symbol->in_phase;
		*quadrature += pulse * symbol->quadrature;
	}
}

static void advance_time(struct sippymodem_qpsk_modulator *modulator)
{
	uint64_t step = modulator->channel.symbol_rate_numerator;
	uint64_t support = modulator->symbol_period_units *
			   modulator->channel.pulse_span_symbols;
	size_t retained = 0;

	for (size_t index = 0; index < modulator->active_count; ++index) {
		struct sippymodem_qpsk_active_symbol symbol =
			modulator->active[index];

		if (symbol.age_units <= support - step) {
			symbol.age_units += step;
			modulator->active[retained++] = symbol;
		}
	}
	modulator->active_count = retained;
	if (step >= modulator->symbol_period_units -
			    modulator->symbol_phase_units) {
		modulator->symbol_phase_units =
			step - (modulator->symbol_period_units -
				modulator->symbol_phase_units);
		modulator->needs_symbol = true;
	} else {
		modulator->symbol_phase_units += step;
	}
	modulator->carrier_phase += modulator->carrier_phase_step;
}

enum sippymodem_qpsk_result sippymodem_qpsk_modulator_sample(
	struct sippymodem_qpsk_modulator *modulator, uint8_t quadrant,
	int16_t *sample, bool *symbol_accepted)
{
	double carrier_phase;
	double in_phase;
	double quadrature;
	double waveform;
	long rounded;

	if (!modulator_is_valid(modulator) || quadrant >= QPSK_QUADRANT_COUNT ||
	    sample == NULL || symbol_accepted == NULL ||
	    (modulator->needs_symbol &&
	     modulator->active_count == SIPPYMODEM_QPSK_MAX_ACTIVE_SYMBOLS))
		return SIPPYMODEM_QPSK_INVALID_ARGUMENT;
	*symbol_accepted = modulator->needs_symbol;
	if (modulator->needs_symbol)
		append_symbol(modulator, quadrant);
	baseband_sample(modulator, &in_phase, &quadrature);
	carrier_phase = (double)modulator->carrier_phase *
			(QPSK_TWO_PI / QPSK_PHASE_SCALE);
	waveform = (in_phase * cos(carrier_phase) -
		    quadrature * sin(carrier_phase)) /
		   modulator->pulse_normalization;
	rounded = lround(waveform * modulator->amplitude);
	if (rounded > modulator->amplitude)
		rounded = modulator->amplitude;
	if (rounded < -modulator->amplitude)
		rounded = -modulator->amplitude;
	*sample = (int16_t)rounded;
	advance_time(modulator);
	return SIPPYMODEM_QPSK_OK;
}

const char *sippymodem_qpsk_result_name(
	enum sippymodem_qpsk_result result)
{
	switch (result) {
	case SIPPYMODEM_QPSK_OK:
		return "ok";
	case SIPPYMODEM_QPSK_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
