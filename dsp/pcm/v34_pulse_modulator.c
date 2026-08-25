// SPDX-License-Identifier: BSD-3-Clause

#include "v34_pulse_modulator.h"

#include "fixed_phase.h"
#include "integer_math.h"
#include "memory_region.h"
#include "phase.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

enum {
	V34_PULSE_SAMPLE_RATE = 8000,
	/* Q12 represents every normalized V.34 data profile while retaining
	 * exact power-of-two lattice coordinates through the fixed line path. */
	V34_PULSE_POINT_SCALE = 4096,
	V34_PULSE_OUTPUT_SCALE = 16384,
};

static const double V34_LINE_ROLLOFF = 0.1;

struct pulse_profile {
	const int16_t *coefficients;
	uint8_t phase_count;
};

#include "v34_pulse_profiles.inc"
#include "v34_preemphasis_profiles.inc"

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
	       channel->sample_rate == expected.sample_rate &&
	       channel->symbol_rate_numerator ==
		       expected.symbol_rate_numerator &&
	       channel->symbol_rate_denominator ==
		       expected.symbol_rate_denominator &&
	       channel->carrier_frequency_numerator ==
		       expected.carrier_frequency_numerator &&
	       channel->carrier_frequency_denominator ==
		       expected.carrier_frequency_denominator &&
	       channel->rolloff == expected.rolloff &&
	       channel->pulse_span_symbols == expected.pulse_span_symbols;
}

static bool channel_matches_profile(
	const struct qam_channel *channel,
	const struct v34_phase3_profile *profile)
{
	return channel != NULL &&
	       channel->sample_rate == V34_PULSE_SAMPLE_RATE &&
	       v34_line_channel_matches(profile, channel);
}

static bool point_fits(
	const struct qam_point *point, double maximum_power)
{
	double power;

	if (!point_is_finite(point) || !isfinite(maximum_power) ||
	    maximum_power <= 0.0)
		return false;
	power = point->in_phase * point->in_phase +
		point->quadrature * point->quadrature;
	return isfinite(power) &&
	       (power <= maximum_power ||
		power / maximum_power <= 1.0 + 1e-12);
}

static const int16_t *selected_preemphasis(
	enum v34_symbol_rate symbol_rate, uint8_t filter_index)
{
	const int16_t (*bank)[V34_PREEMPHASIS_TAPS] = NULL;

	if (filter_index > V34_PREEMPHASIS_MAX_FILTER_INDEX)
		return NULL;
	if (filter_index == 0)
		return V34_PREEMPHASIS_IDENTITY;
	switch (symbol_rate) {
	case V34_SYMBOL_RATE_2400:
		bank = V34_PREEMPHASIS_2400;
		break;
	case V34_SYMBOL_RATE_2743:
	case V34_SYMBOL_RATE_2800:
		bank = V34_PREEMPHASIS_2800;
		break;
	case V34_SYMBOL_RATE_3000:
		bank = V34_PREEMPHASIS_3000;
		break;
	case V34_SYMBOL_RATE_3200:
		bank = V34_PREEMPHASIS_3200;
		break;
	case V34_SYMBOL_RATE_3429:
		bank = V34_PREEMPHASIS_3429;
		break;
	case V34_SYMBOL_RATE_COUNT:
		return NULL;
	}
	return bank != NULL ? bank[filter_index - 1u] : NULL;
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
	uint32_t expected_carrier_phase;

	if (modulator == NULL ||
	    !selected_profile(modulator->symbol_rate, &profile) ||
	    modulator->channel.sample_rate != V34_PULSE_SAMPLE_RATE ||
	    modulator->channel.symbol_rate_numerator == 0 ||
	    modulator->channel.symbol_rate_denominator == 0 ||
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
	if (expected_period == 0 ||
	    modulator->channel.symbol_rate_numerator >= expected_period ||
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

static int16_t clamp_s16(int64_t value)
{
	if (value > INT16_MAX)
		return INT16_MAX;
	if (value < INT16_MIN)
		return INT16_MIN;
	return (int16_t)value;
}

static int16_t interpolated_coefficient(
	const struct pulse_profile *profile, uint32_t target_phase,
	uint32_t target_phase_count, int symbol)
{
	int64_t scaled_phase =
		(int64_t)target_phase * profile->phase_count;
	int phase = (int)(scaled_phase / target_phase_count);
	int64_t fraction = scaled_phase % target_phase_count;
	int64_t scale = target_phase_count;
	int64_t p1 = source_coefficient(profile, phase, symbol);
	int64_t p0;
	int64_t p2;
	int64_t p3;
	int64_t fraction_squared;
	int64_t numerator;
	int64_t denominator;

	if (fraction == 0)
		return (int16_t)p1;
	p0 = source_coefficient(profile, phase - 1, symbol);
	p2 = source_coefficient(profile, phase + 1, symbol);
	p3 = source_coefficient(profile, phase + 2, symbol);
	fraction_squared = fraction * fraction;
	numerator = 2 * p1 * scale * scale * scale;
	numerator += (-p0 + p2) * fraction * scale * scale;
	numerator += (2 * p0 - 5 * p1 + 4 * p2 - p3) *
		fraction_squared * scale;
	numerator += (-p0 + 3 * p1 - 3 * p2 + p3) *
		fraction_squared * fraction;
	denominator = 2 * scale * scale * scale;
	return clamp_s16(divide_round(numerator, denominator));
}

static bool outputs_are_disjoint(
	const struct v34_pulse_modulator *modulator,
	const struct qam_point *point, double *sample,
	bool *point_accepted)
{
	return !memory_regions_overlap(
		       modulator, sizeof(*modulator), sample, sizeof(*sample)) &&
	       !memory_regions_overlap(
		       modulator, sizeof(*modulator), point_accepted,
		       sizeof(*point_accepted)) &&
	       !memory_regions_overlap(
		       sample, sizeof(*sample), point_accepted,
		       sizeof(*point_accepted)) &&
	       (point == NULL ||
		(!memory_regions_overlap(
			 point, sizeof(*point), modulator, sizeof(*modulator)) &&
		 !memory_regions_overlap(
			 point, sizeof(*point), sample, sizeof(*sample)) &&
		 !memory_regions_overlap(
			 point, sizeof(*point), point_accepted,
			 sizeof(*point_accepted))));
}

static int32_t arithmetic_shift_right_s32(uint32_t value, unsigned shift)
{
	uint32_t shifted;

	if (shift == 0)
		shifted = value;
	else if ((value & UINT32_C(0x80000000)) == 0)
		shifted = value >> shift;
	else
		shifted = (value >> shift) | ~(UINT32_MAX >> shift);
	return shifted <= INT32_MAX ?
		       (int32_t)shifted :
		       (int32_t)((int64_t)shifted - (INT64_C(1) << 32));
}

static uint32_t multiply_low_s32(int32_t left, int32_t right)
{
	return (uint32_t)((uint64_t)(uint32_t)left * (uint32_t)right);
}

static int16_t wrap_s16(int32_t value)
{
	uint16_t word = (uint16_t)(uint32_t)value;

	return word <= INT16_MAX ?
		       (int16_t)word : (int16_t)((int32_t)word - 65536);
}

static bool fixed_coordinate(double coordinate, int16_t *fixed)
{
	double scaled = coordinate * V34_PULSE_POINT_SCALE;
	long rounded;

	if (!isfinite(scaled) || scaled < INT16_MIN - 0.5 ||
	    scaled >= INT16_MAX + 0.5)
		return false;
	rounded = lround(scaled);
	if (rounded < INT16_MIN || rounded > INT16_MAX)
		return false;
	*fixed = (int16_t)rounded;
	return true;
}

enum v34_pulse_modulator_result v34_pulse_modulator_sample(
	struct v34_pulse_modulator *modulator,
	const struct qam_point *point, double *sample,
	bool *point_accepted)
{
	struct v34_pulse_modulator next;
	struct pulse_profile profile;
	struct fixed_phase_q14 phasor;
	const int16_t *preemphasis;
	uint32_t divisor;
	uint32_t row_count;
	uint32_t row;
	uint32_t in_phase = 0x1000u;
	uint32_t quadrature = 0x1000u;
	uint32_t mixed;
	uint32_t filtered = 0;
	int16_t fixed_in_phase = 0;
	int16_t fixed_quadrature = 0;
	int16_t preemphasis_input;
	int16_t fixed_sample;
	double generated;
	bool accepting;

	if (modulator == NULL || sample == NULL || point_accepted == NULL ||
	    !outputs_are_disjoint(modulator, point, sample, point_accepted) ||
	    !v34_pulse_modulator_is_valid(modulator))
		return V34_PULSE_MODULATOR_INVALID_ARGUMENT;
	accepting = v34_pulse_modulator_needs_point(modulator);
	if ((point != NULL) != accepting ||
	    (point != NULL &&
	     (!point_fits(point, modulator->maximum_point_power) ||
	      !fixed_coordinate(point->in_phase, &fixed_in_phase) ||
	      !fixed_coordinate(point->quadrature, &fixed_quadrature))))
		return V34_PULSE_MODULATOR_INVALID_ARGUMENT;
	if (modulator->generated_samples == UINT64_MAX ||
	    !selected_profile(modulator->symbol_rate, &profile) ||
	    (preemphasis = selected_preemphasis(
		     modulator->symbol_rate,
		     modulator->preemphasis_filter_index)) == NULL)
		return V34_PULSE_MODULATOR_NUMERIC_FAILURE;
	next = *modulator;
	if (point != NULL) {
		memmove(
			&next.in_phase_history[1], &next.in_phase_history[0],
			(V34_PULSE_TAPS - 1u) *
				sizeof(next.in_phase_history[0]));
		memmove(
			&next.quadrature_history[1],
			&next.quadrature_history[0],
			(V34_PULSE_TAPS - 1u) *
				sizeof(next.quadrature_history[0]));
		next.in_phase_history[0] = fixed_in_phase;
		next.quadrature_history[0] = fixed_quadrature;
	}
	divisor = (uint32_t)gcd_u64(
		next.channel.symbol_rate_numerator,
		next.symbol_period_units);
	row_count = (uint32_t)(next.symbol_period_units / divisor);
	row = (uint32_t)(next.symbol_phase_units / divisor);
	for (size_t index = 0; index < V34_PULSE_TAPS; ++index) {
		int16_t coefficient = interpolated_coefficient(
			&profile, row, row_count,
			V34_PULSE_TAPS - 1 - (int)index);

		in_phase += multiply_low_s32(
			next.in_phase_history[index], coefficient);
		quadrature += multiply_low_s32(
			next.quadrature_history[index], coefficient);
	}
	phasor = fixed_phase_sin_cos_u16_q14(
		(uint16_t)(next.carrier_phase >> 16));
	mixed = multiply_low_s32(
		arithmetic_shift_right_s32(in_phase, 13), phasor.cosine);
	mixed -= multiply_low_s32(
		arithmetic_shift_right_s32(quadrature, 13), phasor.sine);
	preemphasis_input = wrap_s16(
		arithmetic_shift_right_s32(mixed + 0x2000u, 14));
	for (size_t tap = 0; tap < V34_PREEMPHASIS_TAPS; ++tap) {
		int16_t previous = next.preemphasis_history[tap];

		next.preemphasis_history[tap] = preemphasis_input;
		filtered += multiply_low_s32(preemphasis_input, preemphasis[tap]);
		preemphasis_input = previous;
	}
	fixed_sample = wrap_s16(
		arithmetic_shift_right_s32(filtered + 0x2000u, 14));
	generated = (double)fixed_sample / V34_PULSE_OUTPUT_SCALE;
	next.generated_samples++;
	next.carrier_phase += next.carrier_phase_step;
	next.symbol_phase_units += next.channel.symbol_rate_numerator;
	if (next.symbol_phase_units >= next.symbol_period_units)
		next.symbol_phase_units -= next.symbol_period_units;
	if (!v34_pulse_modulator_is_valid(&next))
		return V34_PULSE_MODULATOR_NUMERIC_FAILURE;
	*modulator = next;
	*sample = generated;
	*point_accepted = accepting;
	return V34_PULSE_MODULATOR_OK;
}
