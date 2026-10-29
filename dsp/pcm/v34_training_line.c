// SPDX-License-Identifier: BSD-3-Clause

#include "v34_training_line.h"

#include "datapump_transmit.h"
#include "v34_phase3_s_sequence.h"
#include "memory_region.h"
#include "sample_math.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>

static const double V34_EQUAL_POWER_RMS =
	0.70710678118654752440084436210485;
static const double V34_POWER_TOLERANCE = 1e-9;
static const double V34_TRAINING_MAXIMUM_POINT_POWER = 9.0 / 5.0;

uint16_t v34_training_line_tail_samples(
	const struct v34_phase3_profile *profile)
{
	return (uint16_t)(datapump_symbol_tail(8000,
		profile->symbol_rate_numerator, profile->symbol_rate_denominator,
		V34_TRAINING_LINE_PULSE_TAIL_SYMBOLS + 1u) +
		V34_TRAINING_LINE_FILTER_TAIL_SAMPLES);
}

enum calibration_stage {
	CALIBRATE_SYMBOLS,
	CALIBRATE_PULSE_TAIL,
	CALIBRATE_FILTER_TAIL,
	CALIBRATION_COMPLETE,
};

static uint32_t calibration_sample_count(
	const struct v34_training_line *line)
{
	uint64_t accepted_index = V34_PHASE3_S_SEQUENCE_SYMBOLS +
		V34_TRAINING_LINE_PULSE_TAIL_SYMBOLS - 1u;
	uint64_t numerator = accepted_index *
		line->carrier.channel.sample_rate *
		line->profile.symbol_rate_denominator;
	uint64_t denominator = line->profile.symbol_rate_numerator;
	uint64_t last_acceptance =
		(numerator + denominator - 1u) / denominator;
	uint64_t count = last_acceptance + 1u +
		V34_TRAINING_LINE_FILTER_TAIL_SAMPLES;

	return count <= UINT32_MAX ? (uint32_t)count : 0;
}

static enum v34_training_line_result calibrate_power(
	struct v34_training_line *line)
{
	struct v34_pulse_modulator carrier = line->carrier;
	struct v34_phase3_s_sequence sequence;
	enum calibration_stage stage = CALIBRATE_SYMBOLS;
	long double sum_squares = 0.0L;
	double peak = 0.0;
	uint32_t expected_samples = calibration_sample_count(line);
	uint32_t generated_samples = 0;
	uint16_t tail_symbols = 0;
	uint8_t filter_tail_samples = 0;

	if (expected_samples == 0 || !v34_pulse_modulator_is_valid(&carrier) ||
	    v34_phase3_s_sequence_init(&sequence) !=
		    V34_PHASE3_S_SEQUENCE_OK)
		return V34_TRAINING_LINE_INVALID_ARGUMENT;
	while (stage != CALIBRATION_COMPLETE) {
		struct qam_point point = { 0 };
		enum v34_phase3_s_section section;
		double line_sample;
		double magnitude;
		bool accepting = carrier.symbol_phase_units <
			carrier.channel.symbol_rate_numerator;
		bool sequence_symbol = false;
		bool accepted;
		bool draining_filter = stage == CALIBRATE_FILTER_TAIL;

		if (generated_samples >= expected_samples)
			return V34_TRAINING_LINE_NUMERIC_FAILURE;
		if (stage == CALIBRATE_SYMBOLS && accepting) {
			if (!v34_phase3_s_sequence_is_complete(&sequence)) {
				if (v34_phase3_s_sequence_next(
					    &sequence, &point, &section) !=
				    V34_PHASE3_S_SEQUENCE_OK)
					return V34_TRAINING_LINE_NUMERIC_FAILURE;
				sequence_symbol = true;
			} else {
				stage = CALIBRATE_PULSE_TAIL;
			}
		}
		if (v34_pulse_modulator_sample_owned(
			    &carrier, accepting ? &point : NULL,
			    &line_sample, &accepted) !=
			    V34_PULSE_MODULATOR_OK ||
		    (sequence_symbol && !accepted) ||
		    !isfinite(line_sample))
			return V34_TRAINING_LINE_NUMERIC_FAILURE;
		if (accepted && stage == CALIBRATE_PULSE_TAIL) {
			tail_symbols++;
			if (tail_symbols ==
			    V34_TRAINING_LINE_PULSE_TAIL_SYMBOLS)
				stage = CALIBRATE_FILTER_TAIL;
		}
		if (draining_filter) {
			filter_tail_samples++;
			if (filter_tail_samples ==
			    V34_TRAINING_LINE_FILTER_TAIL_SAMPLES)
				stage = CALIBRATION_COMPLETE;
		}
		magnitude = fabs(line_sample);
		sum_squares += (long double)line_sample * line_sample;
		if (!isfinite(sum_squares))
			return V34_TRAINING_LINE_NUMERIC_FAILURE;
		if (magnitude > peak)
			peak = magnitude;
		generated_samples++;
	}
	if (generated_samples != expected_samples ||
	    !v34_phase3_s_sequence_is_complete(&sequence) ||
	    tail_symbols != V34_TRAINING_LINE_PULSE_TAIL_SYMBOLS ||
	    filter_tail_samples != V34_TRAINING_LINE_FILTER_TAIL_SAMPLES ||
	    peak <= 0.0 || sum_squares <= 0.0L)
		return V34_TRAINING_LINE_NUMERIC_FAILURE;
	line->raw_rms = (double)sqrtl(sum_squares / generated_samples);
	line->raw_peak = peak;
	line->output_gain = line->target_rms / line->raw_rms;
	if (!isfinite(line->raw_rms) || line->raw_rms <= 0.0 ||
	    !isfinite(line->output_gain) || line->output_gain <= 0.0)
		return V34_TRAINING_LINE_NUMERIC_FAILURE;
	if (line->raw_peak * line->output_gain > INT16_MAX - 0.5)
		return V34_TRAINING_LINE_CLIPPING_RISK;
	return V34_TRAINING_LINE_OK;
}

static bool power_metadata_is_valid(
	const struct v34_training_line *line)
{
	double reduction_gain;
	double expected_target;
	double scale;

	if (line->nominal_peak_amplitude <= 0 ||
	    !isfinite(line->output_gain) || line->output_gain <= 0.0 ||
	    !isfinite(line->raw_rms) || line->raw_rms <= 0.0 ||
	    !isfinite(line->raw_peak) || line->raw_peak <= 0.0 ||
	    !isfinite(line->target_rms) || line->target_rms <= 0.0)
		return false;
	reduction_gain = pow(
		10.0,
		-(double)line->profile.transmit_power_reduction_db / 20.0);
	expected_target = line->nominal_peak_amplitude *
		V34_EQUAL_POWER_RMS * reduction_gain;
	scale = fmax(1.0, expected_target);
	return isfinite(expected_target) &&
	       fabs(line->target_rms - expected_target) <=
		       V34_POWER_TOLERANCE * scale &&
	       fabs(line->output_gain * line->raw_rms -
		    line->target_rms) <=
		       V34_POWER_TOLERANCE * scale &&
	       line->raw_peak * line->output_gain <= INT16_MAX - 0.5;
}

bool v34_training_line_is_valid(
	const struct v34_training_line *line)
{
	if (line == NULL ||
	    !v34_pulse_modulator_is_valid(&line->carrier) ||
	    line->carrier.preemphasis_filter_index !=
		    line->profile.preemphasis_filter_index ||
	    !v34_line_channel_matches(
		    &line->profile, &line->carrier.channel) ||
	    !power_metadata_is_valid(line) ||
	    line->carrier.generated_samples != line->generated_samples)
		return false;
	return true;
}

enum v34_training_line_result v34_training_line_init(
	struct v34_training_line *line,
	const struct v34_phase3_profile *profile,
	uint32_t sample_rate, int16_t nominal_peak_amplitude)
{
	struct v34_training_line initialized = { 0 };
	struct qam_channel channel;
	double reduction_gain;
	enum v34_line_channel_result channel_result;
	enum v34_training_line_result result;

	if (line == NULL || !v34_phase3_profile_is_valid(profile) ||
	    sample_rate == 0 || nominal_peak_amplitude <= 0)
		return V34_TRAINING_LINE_INVALID_ARGUMENT;
	initialized.profile = *profile;
	initialized.nominal_peak_amplitude = nominal_peak_amplitude;
	reduction_gain = pow(
		10.0, -(double)profile->transmit_power_reduction_db / 20.0);
	initialized.target_rms = nominal_peak_amplitude *
		V34_EQUAL_POWER_RMS * reduction_gain;
	channel_result = v34_line_channel_init(
		profile, sample_rate, &channel);
	if (channel_result != V34_LINE_CHANNEL_OK)
		return channel_result == V34_LINE_CHANNEL_OUT_OF_RANGE ?
		       V34_TRAINING_LINE_OUT_OF_RANGE :
		       V34_TRAINING_LINE_INVALID_ARGUMENT;
	if (!isfinite(initialized.target_rms) ||
	    initialized.target_rms <= 0.0 ||
	    v34_pulse_modulator_init(
		    &initialized.carrier, profile, &channel,
		    V34_TRAINING_MAXIMUM_POINT_POWER) !=
		    V34_PULSE_MODULATOR_OK)
		return V34_TRAINING_LINE_INVALID_ARGUMENT;
	result = calibrate_power(&initialized);
	if (result != V34_TRAINING_LINE_OK)
		return result;
	if (!v34_training_line_is_valid(&initialized))
		return V34_TRAINING_LINE_NUMERIC_FAILURE;
	*line = initialized;
	return V34_TRAINING_LINE_OK;
}

bool v34_training_line_needs_point(
	const struct v34_training_line *line)
{
	return v34_training_line_is_valid(line) &&
	       line->carrier.symbol_phase_units <
		       line->carrier.channel.symbol_rate_numerator;
}

enum v34_training_line_result v34_training_line_sample(
	struct v34_training_line *line,
	const struct qam_point *point, int16_t *sample,
	bool *point_accepted)
{
	struct v34_pulse_modulator next;
	const struct memory_region regions[] = {
		{ line, sizeof(*line) },
		{ sample, sizeof(*sample) },
		{ point_accepted, sizeof(*point_accepted) },
		{ point, sizeof(*point) },
	};
	double line_sample;
	double scaled_sample;
	bool accepting;
	bool accepted;

	if (sample == NULL || point_accepted == NULL ||
	    memory_region_set_overlaps(regions, point == NULL ? 3u : 4u) ||
	    !v34_training_line_is_valid(line))
		return V34_TRAINING_LINE_INVALID_ARGUMENT;
	accepting = line->carrier.symbol_phase_units <
		line->carrier.channel.symbol_rate_numerator;
	if ((point != NULL) != accepting)
		return V34_TRAINING_LINE_INVALID_ARGUMENT;
	if (line->generated_samples == UINT64_MAX)
		return V34_TRAINING_LINE_OUT_OF_RANGE;
	next = line->carrier;
	if (v34_pulse_modulator_sample_owned(
		    &next, point,
		    &line_sample, &accepted) != V34_PULSE_MODULATOR_OK)
		return point != NULL ? V34_TRAINING_LINE_INVALID_ARGUMENT :
				       V34_TRAINING_LINE_NUMERIC_FAILURE;
	scaled_sample = line_sample * line->output_gain;
	if (!isfinite(scaled_sample))
		return V34_TRAINING_LINE_NUMERIC_FAILURE;
	if (scaled_sample < INT16_MIN - 0.499 ||
	    scaled_sample > INT16_MAX + 0.499)
		return V34_TRAINING_LINE_CLIPPING_RISK;
	/* The pulse transition preserves its clocks and channel. Calibration
	 * stays fixed, and the headroom check makes rounding representable. */
	line->carrier = next;
	line->generated_samples = next.generated_samples;
	*sample = sample_round_s16(scaled_sample);
	*point_accepted = accepted;
	return V34_TRAINING_LINE_OK;
}
