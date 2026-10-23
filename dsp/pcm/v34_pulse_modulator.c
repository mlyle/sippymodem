// SPDX-License-Identifier: BSD-3-Clause

#include "v34_pulse_modulator.h"

#include "fixed_phase.h"
#include "fir.h"
#include "integer_math.h"
#include "memory_region.h"
#include "sample_math.h"
#include "phase.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

enum {
	V34_PULSE_SAMPLE_RATE = 8000,
	V34_PULSE_OUTPUT_SCALE = 16384,
};

static const double V34_LINE_ROLLOFF = 0.1;

struct pulse_profile {
	const int16_t *coefficients;
	uint8_t phase_count;
};

#include "v34_pulse_profiles.inc"
#include "v34_preemphasis_profiles.h"

static const int16_t V34_PREEMPHASIS_IDENTITY[V34_PREEMPHASIS_TAPS] = {
	[7] = V34_PULSE_OUTPUT_SCALE,
};

static bool selected_profile(
	enum v34_symbol_rate symbol_rate,
	struct pulse_profile *profile)
{
	switch (symbol_rate) {
	case V34_SYMBOL_RATE_2400:
		*profile = (struct pulse_profile){
			v34_pulse_coefficients_2400, 4,
		};
		return true;
	case V34_SYMBOL_RATE_2743:
	case V34_SYMBOL_RATE_2800:
		*profile = (struct pulse_profile){
			v34_pulse_coefficients_2800, 24,
		};
		return true;
	case V34_SYMBOL_RATE_3000:
		*profile = (struct pulse_profile){
			v34_pulse_coefficients_3000, 16,
		};
		return true;
	case V34_SYMBOL_RATE_3200:
		*profile = (struct pulse_profile){
			v34_pulse_coefficients_3200_v34, 3,
		};
		return true;
	case V34_SYMBOL_RATE_3429:
		*profile = (struct pulse_profile){
			v34_pulse_coefficients_3429, 14,
		};
		return true;
	case V34_SYMBOL_RATE_COUNT:
		break;
	}
	return false;
}

static bool occupied_band_fits(
	const struct v34_phase3_profile *profile,
	uint32_t sample_rate)
{
	long double carrier =
		(long double)profile->carrier_frequency_numerator /
		profile->carrier_frequency_denominator;
	long double symbol_rate =
		(long double)profile->symbol_rate_numerator /
		profile->symbol_rate_denominator;
	long double upper_edge = carrier +
		0.5L * (1.0L + V34_LINE_ROLLOFF) * symbol_rate;

	return upper_edge < 0.5L * sample_rate;
}

enum v34_line_channel_result v34_line_channel_init(
	const struct v34_phase3_profile *profile,
	uint32_t sample_rate, struct qam_channel *channel)
{
	struct qam_channel configured;

	if (!v34_phase3_profile_is_valid(profile) || sample_rate == 0 ||
	    channel == NULL)
		return V34_LINE_CHANNEL_INVALID_ARGUMENT;
	if (!occupied_band_fits(profile, sample_rate))
		return V34_LINE_CHANNEL_OUT_OF_RANGE;
	configured = (struct qam_channel){
		.sample_rate = sample_rate,
		.symbol_rate_numerator = profile->symbol_rate_numerator,
		.symbol_rate_denominator = profile->symbol_rate_denominator,
		.carrier_frequency_numerator =
			profile->carrier_frequency_numerator,
		.carrier_frequency_denominator =
			profile->carrier_frequency_denominator,
		.rolloff = V34_LINE_ROLLOFF,
		.pulse_span_symbols = V34_LINE_PULSE_SPAN_SYMBOLS,
	};
	*channel = configured;
	return V34_LINE_CHANNEL_OK;
}

bool v34_line_channel_matches(
	const struct v34_phase3_profile *profile,
	const struct qam_channel *channel)
{
	struct qam_channel expected;

	return channel != NULL &&
	       v34_line_channel_init(
		       profile, channel->sample_rate, &expected) ==
		       V34_LINE_CHANNEL_OK &&
	       qam_channels_match(channel, &expected);
}

static bool channel_matches_profile(
	const struct qam_channel *channel,
	const struct v34_phase3_profile *profile)
{
	return channel != NULL &&
	       channel->sample_rate == V34_PULSE_SAMPLE_RATE &&
	       v34_line_channel_matches(profile, channel);
}

static const int16_t *selected_preemphasis(
	enum v34_symbol_rate symbol_rate, uint8_t filter_index)
{
	if ((unsigned int)symbol_rate >= V34_SYMBOL_RATE_COUNT ||
	    filter_index > V34_PREEMPHASIS_MAX_FILTER_INDEX)
		return NULL;
	if (filter_index == 0)
		return V34_PREEMPHASIS_IDENTITY;
	return V34_PREEMPHASIS_PROFILES[symbol_rate][filter_index - 1u];
}

static bool point_range_is_representable(double maximum_power)
{
	double maximum_coordinate;

	if (!isfinite(maximum_power) || maximum_power <= 0.0)
		return false;
	maximum_coordinate = sqrt(maximum_power) * V34_PULSE_POINT_SCALE;
	return isfinite(maximum_coordinate) &&
	       maximum_coordinate < INT16_MAX + 0.5;
}

bool v34_pulse_modulator_is_valid(
	const struct v34_pulse_modulator *modulator)
{
	struct pulse_profile profile;
	uint64_t expected_period;
	uint64_t expected_phase;
	uint32_t expected_divisor;
	uint32_t expected_carrier_phase;

	if (modulator == NULL ||
	    !selected_profile(modulator->symbol_rate, &profile) ||
	    modulator->channel.sample_rate != V34_PULSE_SAMPLE_RATE ||
	    !v34_symbol_rate_matches(
		    modulator->symbol_rate,
		    modulator->channel.symbol_rate_numerator,
		    modulator->channel.symbol_rate_denominator) ||
	    modulator->channel.carrier_frequency_numerator == 0 ||
	    modulator->channel.carrier_frequency_denominator == 0 ||
	    modulator->channel.rolloff != V34_LINE_ROLLOFF ||
	    modulator->channel.pulse_span_symbols !=
		    V34_LINE_PULSE_SPAN_SYMBOLS ||
	    !point_range_is_representable(modulator->maximum_point_power) ||
	    selected_preemphasis(
		    modulator->symbol_rate,
		    modulator->preemphasis_filter_index) == NULL)
		return false;
	expected_period = (uint64_t)modulator->channel.sample_rate *
		modulator->channel.symbol_rate_denominator;
	expected_divisor = (uint32_t)gcd_u64(
		modulator->channel.symbol_rate_numerator, expected_period);
	if (modulator->pulse_phase_divisor != expected_divisor ||
	    modulator->pulse_phase_count != expected_period / expected_divisor ||
	    modulator->symbol_period_units != expected_period ||
	    modulator->carrier_phase_step != phase_step_u32(
		    modulator->channel.sample_rate,
		    modulator->channel.carrier_frequency_numerator,
		    modulator->channel.carrier_frequency_denominator) ||
	    modulator->carrier_phase_step == 0 ||
	    (uint64_t)modulator->channel.carrier_frequency_numerator * 2u >=
		    (uint64_t)modulator->channel.sample_rate *
		    modulator->channel.carrier_frequency_denominator)
		return false;
	expected_phase =
		(modulator->generated_samples % expected_period) *
		modulator->channel.symbol_rate_numerator % expected_period;
	expected_carrier_phase =
		(uint32_t)modulator->generated_samples *
		modulator->carrier_phase_step;
	return modulator->symbol_phase_units == expected_phase &&
	       modulator->carrier_phase == expected_carrier_phase;
}

enum v34_pulse_modulator_result v34_pulse_modulator_init(
	struct v34_pulse_modulator *modulator,
	const struct v34_phase3_profile *profile,
	const struct qam_channel *channel, double maximum_point_power)
{
	struct v34_pulse_modulator initialized = { 0 };
	struct pulse_profile selected;

	if (modulator == NULL || !v34_phase3_profile_is_valid(profile) ||
	    !selected_profile(profile->symbol_rate, &selected) ||
	    !channel_matches_profile(channel, profile) ||
	    !point_range_is_representable(maximum_point_power) ||
	    selected_preemphasis(
		    profile->symbol_rate,
		    profile->preemphasis_filter_index) == NULL)
		return V34_PULSE_MODULATOR_INVALID_ARGUMENT;
	initialized.channel = *channel;
	initialized.symbol_period_units =
		(uint64_t)channel->sample_rate *
		channel->symbol_rate_denominator;
	initialized.carrier_phase_step = phase_step_u32(
		channel->sample_rate, channel->carrier_frequency_numerator,
		channel->carrier_frequency_denominator);
	initialized.pulse_phase_divisor = (uint32_t)gcd_u64(
		channel->symbol_rate_numerator, initialized.symbol_period_units);
	initialized.pulse_phase_count = (uint32_t)(initialized.symbol_period_units /
		initialized.pulse_phase_divisor);
	initialized.maximum_point_power = maximum_point_power;
	initialized.symbol_rate = profile->symbol_rate;
	initialized.preemphasis_filter_index =
		profile->preemphasis_filter_index;
	if (!v34_pulse_modulator_is_valid(&initialized))
		return V34_PULSE_MODULATOR_INVALID_ARGUMENT;
	*modulator = initialized;
	return V34_PULSE_MODULATOR_OK;
}

bool v34_pulse_modulator_needs_point(
	const struct v34_pulse_modulator *modulator)
{
	return v34_pulse_modulator_is_valid(modulator) &&
	       modulator->symbol_phase_units <
		       modulator->channel.symbol_rate_numerator;
}

static int16_t source_coefficient(
	const struct pulse_profile *profile, int phase, int symbol)
{
	if (phase < 0) {
		phase += profile->phase_count;
		symbol++;
	} else if (phase >= profile->phase_count) {
		phase -= profile->phase_count;
		symbol--;
	}
	if (symbol < 0 || symbol >= V34_PULSE_TAPS)
		return 0;
	return profile->coefficients[
		(size_t)phase * V34_PULSE_TAPS + (size_t)symbol];
}

static int64_t divide_round(int64_t numerator, int64_t denominator)
{
	return numerator >= 0 ?
		       (numerator + denominator / 2) / denominator :
		       -((-numerator + denominator / 2) / denominator);
}

static const int16_t *phase_coefficients(
	const struct pulse_profile *profile, uint32_t target_phase,
	uint32_t target_phase_count, int16_t interpolated[V34_PULSE_TAPS])
{
	int64_t scaled_phase =
		(int64_t)target_phase * profile->phase_count;
	int phase = (int)(scaled_phase / target_phase_count);
	int64_t fraction = scaled_phase % target_phase_count;
	int64_t scale = target_phase_count;

	if (fraction == 0)
		return &profile->coefficients[(size_t)phase * V34_PULSE_TAPS];

	/* The cubic phase weights are shared by every tap in this sample. */
	int64_t denominator = 2 * scale * scale * scale;
	int64_t linear = fraction * scale * scale;
	int64_t quadratic = fraction * fraction * scale;
	int64_t cubic = fraction * fraction * fraction;

	for (int symbol = 0; symbol < V34_PULSE_TAPS; ++symbol) {
		int64_t p0 = source_coefficient(profile, phase - 1, symbol);
		int64_t p1 = source_coefficient(profile, phase, symbol);
		int64_t p2 = source_coefficient(profile, phase + 1, symbol);
		int64_t p3 = source_coefficient(profile, phase + 2, symbol);
		int64_t numerator = p1 * denominator + (-p0 + p2) * linear +
			(2 * p0 - 5 * p1 + 4 * p2 - p3) * quadratic +
			(-p0 + 3 * p1 - 3 * p2 + p3) * cubic;

		interpolated[symbol] = clamp_s16(divide_round(numerator, denominator));
	}
	return interpolated;
}

static bool outputs_are_disjoint(
	const struct v34_pulse_modulator *modulator,
	const struct qam_point *point, double *sample,
	bool *point_accepted)
{
	const struct memory_region regions[] = {
		{ modulator, sizeof(*modulator) },
		{ sample, sizeof(*sample) },
		{ point_accepted, sizeof(*point_accepted) },
		{ point, sizeof(*point) },
	};
	size_t count = point == NULL ? 3u : 4u;

	return !memory_region_set_overlaps(regions, count);
}

static enum v34_pulse_modulator_result pulse_sample(
	struct v34_pulse_modulator *modulator,
	const struct qam_point *point, double *sample,
	bool *point_accepted, bool validate)
{
	struct pulse_profile profile;
	struct fixed_phase_q14 phasor;
	int16_t interpolated[V34_PULSE_TAPS];
	const int16_t *coefficients;
	const int16_t *preemphasis;
	uint32_t row;
	uint32_t in_phase = 0x1000u;
	uint32_t quadrature = 0x1000u;
	uint32_t mixed;
	uint32_t filtered;
	uint64_t next_symbol_phase;
	int16_t fixed_in_phase = 0;
	int16_t fixed_quadrature = 0;
	int16_t preemphasis_input;
	int16_t fixed_sample;
	double generated;
	bool accepting;

	if (modulator == NULL || sample == NULL || point_accepted == NULL ||
	    !outputs_are_disjoint(modulator, point, sample, point_accepted) ||
	    (validate && !v34_pulse_modulator_is_valid(modulator)))
		return V34_PULSE_MODULATOR_INVALID_ARGUMENT;
	accepting = modulator->symbol_phase_units <
		modulator->channel.symbol_rate_numerator;
	if ((point != NULL) != accepting ||
	    (point != NULL &&
	     (!qam_point_fits_power(point, modulator->maximum_point_power) ||
	      !sample_round_checked_s16(point->in_phase * V34_PULSE_POINT_SCALE,
				     &fixed_in_phase) ||
	      !sample_round_checked_s16(point->quadrature * V34_PULSE_POINT_SCALE,
				     &fixed_quadrature))))
		return V34_PULSE_MODULATOR_INVALID_ARGUMENT;
	if (modulator->generated_samples == UINT64_MAX ||
	    modulator->pulse_phase_divisor == 0 || modulator->pulse_phase_count == 0 ||
	    !selected_profile(modulator->symbol_rate, &profile) ||
	    (preemphasis = selected_preemphasis(
		     modulator->symbol_rate,
		     modulator->preemphasis_filter_index)) == NULL)
		return V34_PULSE_MODULATOR_NUMERIC_FAILURE;
	/* A valid owner has a bounded V.34 clock. After the checks above,
	 * filtering and unsigned clock updates cannot reject the sample. */
	next_symbol_phase = modulator->symbol_phase_units +
		modulator->channel.symbol_rate_numerator;
	if (next_symbol_phase >= modulator->symbol_period_units)
		next_symbol_phase -= modulator->symbol_period_units;
	if (point != NULL) {
		memmove(
			&modulator->in_phase_history[1],
			&modulator->in_phase_history[0],
			(V34_PULSE_TAPS - 1u) *
				sizeof(modulator->in_phase_history[0]));
		memmove(
			&modulator->quadrature_history[1],
			&modulator->quadrature_history[0],
			(V34_PULSE_TAPS - 1u) *
				sizeof(modulator->quadrature_history[0]));
		modulator->in_phase_history[0] = fixed_in_phase;
		modulator->quadrature_history[0] = fixed_quadrature;
	}
	row = (uint32_t)(modulator->symbol_phase_units / modulator->pulse_phase_divisor);
	coefficients = phase_coefficients(
		&profile, row, modulator->pulse_phase_count, interpolated);
	for (size_t index = 0; index < V34_PULSE_TAPS; ++index) {
		int16_t coefficient = coefficients[V34_PULSE_TAPS - 1u - index];

		in_phase += multiply_low_s32(
			modulator->in_phase_history[index], coefficient);
		quadrature += multiply_low_s32(
			modulator->quadrature_history[index], coefficient);
	}
	phasor = fixed_phase_sin_cos_u16_q14(
		(uint16_t)(modulator->carrier_phase >> 16));
	mixed = multiply_low_s32(
		arithmetic_shift_right_u32(in_phase, 13), phasor.cosine);
	mixed -= multiply_low_s32(
		arithmetic_shift_right_u32(quadrature, 13), phasor.sine);
	preemphasis_input = round_shift_s32_word_to_s16(mixed, 14);
	filtered = fir_shift_s16(modulator->preemphasis_history, preemphasis,
		V34_PREEMPHASIS_TAPS, preemphasis_input);
	fixed_sample = round_shift_s32_word_to_s16(filtered, 14);
	generated = (double)fixed_sample / V34_PULSE_OUTPUT_SCALE;
	modulator->generated_samples++;
	modulator->carrier_phase += modulator->carrier_phase_step;
	modulator->symbol_phase_units = next_symbol_phase;
	*sample = generated;
	*point_accepted = accepting;
	return V34_PULSE_MODULATOR_OK;
}

enum v34_pulse_modulator_result v34_pulse_modulator_sample(
	struct v34_pulse_modulator *modulator,
	const struct qam_point *point, double *sample,
	bool *point_accepted)
{
	return pulse_sample(modulator, point, sample, point_accepted, true);
}

enum v34_pulse_modulator_result v34_pulse_modulator_sample_owned(
	struct v34_pulse_modulator *modulator,
	const struct qam_point *point, double *sample,
	bool *point_accepted)
{
	return pulse_sample(modulator, point, sample, point_accepted, false);
}
