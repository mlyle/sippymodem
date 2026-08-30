// SPDX-License-Identifier: BSD-3-Clause

#include "v34_training_line.h"

#include "v34_phase3_s_sequence.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>

static const double V34_TRAINING_MAXIMUM_POINT_POWER = 9.0 / 5.0;

enum {
	/* The remaining constants are the initial Q14 scale, 1 dB reduction,
	 * and final line-level factor. */
	V34_FIXED_TX_SCALE_Q14 = 0x16a1,
	V34_FIXED_REDUCTION_1_DB_Q14 = 0x390a,
	V34_FIXED_LINE_LEVEL_Q14 = 0x4b4b,
	V34_FIXED_LINE_LEVEL_ROUNDING = 0x2000,
};

static int32_t fixed_line_scale(uint8_t reduction_db)
{
	uint32_t scale = V34_FIXED_TX_SCALE_Q14;

	for (uint8_t step = 0; step < reduction_db; ++step)
		scale = scale * V34_FIXED_REDUCTION_1_DB_Q14 >> 14;
	return (int32_t)(
		(scale * V34_FIXED_LINE_LEVEL_Q14 +
		 V34_FIXED_LINE_LEVEL_ROUNDING) >> 14);
}

static double fixed_output_gain(
	uint8_t reduction_db, int16_t nominal_peak_amplitude)
{
	return (double)fixed_line_scale(reduction_db) *
		V34_TRAINING_FIXED_POINT_ZERO /
		V34_TRAINING_PULSE_POINT_ZERO_Q12 *
		nominal_peak_amplitude /
		V34_TRAINING_LINE_REFERENCE_AMPLITUDE;
}

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

static enum v34_training_line_result validate_training_peak(
	const struct v34_training_line *line)
{
	struct v34_pulse_modulator carrier = line->carrier;
	struct v34_phase3_s_sequence sequence;
	double peak = 0.0;
	uint32_t expected_samples = calibration_sample_count(line);

	if (expected_samples == 0 ||
	    v34_phase3_s_sequence_init(&sequence) !=
		    V34_PHASE3_S_SEQUENCE_OK)
		return V34_TRAINING_LINE_INVALID_ARGUMENT;
	for (uint32_t generated = 0; generated < expected_samples;
	     ++generated) {
		struct qam_point point = { 0 };
		enum v34_phase3_s_section section;
		double line_sample;
		bool accepting = v34_pulse_modulator_needs_point(&carrier);
		bool accepted;

		if (accepting &&
		    !v34_phase3_s_sequence_is_complete(&sequence) &&
		    v34_phase3_s_sequence_next(
			    &sequence, &point, &section) !=
			    V34_PHASE3_S_SEQUENCE_OK)
			return V34_TRAINING_LINE_NUMERIC_FAILURE;
		if (v34_pulse_modulator_sample(
			    &carrier, accepting ? &point : NULL,
			    &line_sample, &accepted) !=
			    V34_PULSE_MODULATOR_OK ||
		    accepted != accepting ||
		    !isfinite(line_sample))
			return V34_TRAINING_LINE_NUMERIC_FAILURE;
		if (fabs(line_sample) > peak)
			peak = fabs(line_sample);
	}
	if (!v34_phase3_s_sequence_is_complete(&sequence) || peak <= 0.0)
		return V34_TRAINING_LINE_NUMERIC_FAILURE;
	if (peak * line->output_gain > INT16_MAX - 0.5)
		return V34_TRAINING_LINE_CLIPPING_RISK;
	return V34_TRAINING_LINE_OK;
}

static bool line_gain_is_valid(
	const struct v34_training_line *line)
{
	double expected_gain;

	if (line->nominal_peak_amplitude <= 0 ||
	    !isfinite(line->output_gain) || line->output_gain <= 0.0)
		return false;
	expected_gain = fixed_output_gain(
		line->profile.transmit_power_reduction_db,
		line->nominal_peak_amplitude);
	return line->output_gain == expected_gain;
}

bool v34_training_line_is_valid(
	const struct v34_training_line *line)
{
	if (line == NULL ||
	    !v34_phase3_profile_is_valid(&line->profile) ||
	    !v34_pulse_modulator_is_valid(&line->carrier) ||
	    !v34_line_channel_matches(
		    &line->profile, &line->carrier.channel) ||
	    !line_gain_is_valid(line) ||
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
	enum v34_line_channel_result channel_result;
	enum v34_training_line_result result;

	if (line == NULL || !v34_phase3_profile_is_valid(profile) ||
	    sample_rate == 0 || nominal_peak_amplitude <= 0)
		return V34_TRAINING_LINE_INVALID_ARGUMENT;
	initialized.profile = *profile;
	initialized.nominal_peak_amplitude = nominal_peak_amplitude;
	initialized.output_gain = fixed_output_gain(
		profile->transmit_power_reduction_db,
		nominal_peak_amplitude);
	channel_result = v34_line_channel_init(
		profile, sample_rate, &channel);
	if (channel_result != V34_LINE_CHANNEL_OK)
		return channel_result == V34_LINE_CHANNEL_OUT_OF_RANGE ?
		       V34_TRAINING_LINE_OUT_OF_RANGE :
		       V34_TRAINING_LINE_INVALID_ARGUMENT;
	if (!isfinite(initialized.output_gain) ||
	    initialized.output_gain <= 0.0 ||
	    v34_pulse_modulator_init(
		    &initialized.carrier, profile, &channel,
		    V34_TRAINING_MAXIMUM_POINT_POWER) !=
		    V34_PULSE_MODULATOR_OK)
		return V34_TRAINING_LINE_INVALID_ARGUMENT;
	result = validate_training_peak(&initialized);
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
	       v34_pulse_modulator_needs_point(&line->carrier);
}

enum v34_training_line_result v34_training_line_sample(
	struct v34_training_line *line,
	const struct qam_point *point, int16_t *sample,
	bool *point_accepted)
{
	struct v34_training_line next;
	double line_sample;
	double scaled_sample;
	long rounded;
	bool accepting;
	bool accepted;

	if (!v34_training_line_is_valid(line) || sample == NULL ||
	    point_accepted == NULL)
		return V34_TRAINING_LINE_INVALID_ARGUMENT;
	accepting = v34_pulse_modulator_needs_point(&line->carrier);
	if ((point != NULL) != accepting)
		return V34_TRAINING_LINE_INVALID_ARGUMENT;
	if (line->generated_samples == UINT64_MAX)
		return V34_TRAINING_LINE_OUT_OF_RANGE;
	next = *line;
	if (v34_pulse_modulator_sample(
		    &next.carrier, point,
		    &line_sample, &accepted) != V34_PULSE_MODULATOR_OK)
		return point != NULL ? V34_TRAINING_LINE_INVALID_ARGUMENT :
				       V34_TRAINING_LINE_NUMERIC_FAILURE;
	if (accepted != accepting)
		return V34_TRAINING_LINE_NUMERIC_FAILURE;
	scaled_sample = line_sample * next.output_gain;
	if (!isfinite(scaled_sample))
		return V34_TRAINING_LINE_NUMERIC_FAILURE;
	if (scaled_sample < INT16_MIN - 0.499 ||
	    scaled_sample > INT16_MAX + 0.499)
		return V34_TRAINING_LINE_CLIPPING_RISK;
	rounded = lround(scaled_sample);
	if (rounded < INT16_MIN || rounded > INT16_MAX)
		return V34_TRAINING_LINE_CLIPPING_RISK;
	next.generated_samples++;
	if (!v34_training_line_is_valid(&next))
		return V34_TRAINING_LINE_NUMERIC_FAILURE;
	*line = next;
	*sample = (int16_t)rounded;
	*point_accepted = accepted;
	return V34_TRAINING_LINE_OK;
}
