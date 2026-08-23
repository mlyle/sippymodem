// SPDX-License-Identifier: BSD-3-Clause

#include "v34_pulse_modulator.h"

#include "integer_math.h"
#include "memory_region.h"
#include "phase.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

enum {
	V34_PULSE_SAMPLE_RATE = 8000,
	V34_PULSE_Q14_SCALE = 16384,
};

struct pulse_profile {
	const int16_t *coefficients;
	uint8_t phase_count;
};

#include "v34_pulse_profiles.inc"

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

static bool channel_matches_profile(
	const struct qam_channel *channel,
	const struct v34_phase3_profile *profile)
{
	return channel != NULL && profile != NULL &&
	       channel->sample_rate == V34_PULSE_SAMPLE_RATE &&
	       channel->symbol_rate_numerator ==
		       profile->symbol_rate_numerator &&
	       channel->symbol_rate_denominator ==
		       profile->symbol_rate_denominator &&
	       channel->carrier_frequency_numerator ==
		       profile->carrier_frequency_numerator &&
	       channel->carrier_frequency_denominator ==
		       profile->carrier_frequency_denominator &&
	       channel->rolloff == 0.1 &&
	       channel->pulse_span_symbols == 14;
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

static bool history_is_valid(
	const struct v34_pulse_modulator *modulator)
{
	for (size_t index = 0; index < V34_PULSE_TAPS; ++index)
		if (!point_fits(
			    &modulator->history[index],
			    modulator->maximum_point_power))
			return false;
	return true;
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
	    modulator->channel.rolloff != 0.1 ||
	    modulator->channel.pulse_span_symbols != 14 ||
	    modulator->amplitude <= 0 ||
	    !isfinite(modulator->maximum_point_power) ||
	    modulator->maximum_point_power <= 0.0)
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
			    modulator->channel.carrier_frequency_denominator ||
	    !history_is_valid(modulator))
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
	const struct qam_channel *channel, int16_t amplitude,
	double maximum_point_power)
{
	struct v34_pulse_modulator initialized = { 0 };
	struct pulse_profile selected;

	if (modulator == NULL || !v34_phase3_profile_is_valid(profile) ||
	    !selected_profile(profile->symbol_rate, &selected) ||
	    !channel_matches_profile(channel, profile) || amplitude <= 0 ||
	    !isfinite(maximum_point_power) || maximum_point_power <= 0.0)
		return V34_PULSE_MODULATOR_INVALID_ARGUMENT;
	initialized.channel = *channel;
	initialized.symbol_period_units =
		(uint64_t)channel->sample_rate *
		channel->symbol_rate_denominator;
	initialized.carrier_phase_step = phase_step_u32(
		channel->sample_rate, channel->carrier_frequency_numerator,
		channel->carrier_frequency_denominator);
	initialized.maximum_point_power = maximum_point_power;
	initialized.amplitude = amplitude;
	initialized.symbol_rate = profile->symbol_rate;
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

enum v34_pulse_modulator_result v34_pulse_modulator_sample(
	struct v34_pulse_modulator *modulator,
	const struct qam_point *point, double *sample,
	bool *point_accepted)
{
	struct v34_pulse_modulator next;
	struct pulse_profile profile;
	uint32_t divisor;
	uint32_t row_count;
	uint32_t row;
	double in_phase = 0.0;
	double quadrature = 0.0;
	double carrier_phase;
	double generated;
	bool accepting;

	if (modulator == NULL || sample == NULL || point_accepted == NULL ||
	    !outputs_are_disjoint(modulator, point, sample, point_accepted) ||
	    !v34_pulse_modulator_is_valid(modulator))
		return V34_PULSE_MODULATOR_INVALID_ARGUMENT;
	accepting = v34_pulse_modulator_needs_point(modulator);
	if ((point != NULL) != accepting ||
	    (point != NULL &&
	     !point_fits(point, modulator->maximum_point_power)))
		return V34_PULSE_MODULATOR_INVALID_ARGUMENT;
	if (modulator->generated_samples == UINT64_MAX ||
	    !selected_profile(modulator->symbol_rate, &profile))
		return V34_PULSE_MODULATOR_NUMERIC_FAILURE;
	next = *modulator;
	if (point != NULL) {
		memmove(
			&next.history[1], &next.history[0],
			(V34_PULSE_TAPS - 1u) * sizeof(next.history[0]));
		next.history[0] = *point;
	}
	divisor = (uint32_t)gcd_u64(
		next.channel.symbol_rate_numerator,
		next.symbol_period_units);
	row_count = (uint32_t)(next.symbol_period_units / divisor);
	row = (uint32_t)(next.symbol_phase_units / divisor);
	for (size_t index = 0; index < V34_PULSE_TAPS; ++index) {
		double coefficient = interpolated_coefficient(
			&profile, row, row_count,
			V34_PULSE_TAPS - 1 - (int)index);

		in_phase += next.history[index].in_phase * coefficient /
			V34_PULSE_Q14_SCALE;
		quadrature += next.history[index].quadrature * coefficient /
			V34_PULSE_Q14_SCALE;
	}
	carrier_phase = phase_radians_u32(next.carrier_phase);
	generated = in_phase * cos(carrier_phase) -
		quadrature * sin(carrier_phase);
	if (!isfinite(in_phase) || !isfinite(quadrature) ||
	    !isfinite(generated))
		return V34_PULSE_MODULATOR_NUMERIC_FAILURE;
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
