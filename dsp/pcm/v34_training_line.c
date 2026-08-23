// SPDX-License-Identifier: BSD-3-Clause

#include "v34_training_line.h"

#include "v34_phase3_s_sequence.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>

static const double V34_TRAINING_ROLLOFF = 0.1;
static const double V34_EQUAL_POWER_RMS =
	0.70710678118654752440084436210485;
static const double V34_POWER_TOLERANCE = 1e-9;
static const double V34_TRAINING_MAXIMUM_POINT_POWER = 9.0 / 5.0;

enum calibration_stage {
	CALIBRATE_SYMBOLS,
	CALIBRATE_PULSE_TAIL,
	CALIBRATE_FILTER_TAIL,
	CALIBRATION_COMPLETE,
};

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
		0.5L * (1.0L + V34_TRAINING_ROLLOFF) * symbol_rate;

	return upper_edge < 0.5L * sample_rate;
}

enum v34_training_line_result v34_training_line_channel(
	const struct v34_phase3_profile *profile,
	uint32_t sample_rate, struct qam_channel *channel)
{
	struct qam_channel configured;

	if (!v34_phase3_profile_is_valid(profile) || sample_rate == 0 ||
	    channel == NULL)
		return V34_TRAINING_LINE_INVALID_ARGUMENT;
	if (!occupied_band_fits(profile, sample_rate))
		return V34_TRAINING_LINE_OUT_OF_RANGE;
	configured = (struct qam_channel){
		.sample_rate = sample_rate,
		.symbol_rate_numerator = profile->symbol_rate_numerator,
		.symbol_rate_denominator = profile->symbol_rate_denominator,
		.carrier_frequency_numerator =
			profile->carrier_frequency_numerator,
		.carrier_frequency_denominator =
			profile->carrier_frequency_denominator,
		.rolloff = V34_TRAINING_ROLLOFF,
		.pulse_span_symbols =
			V34_TRAINING_LINE_PULSE_SPAN_SYMBOLS,
	};
	*channel = configured;
	return V34_TRAINING_LINE_OK;
}

bool v34_training_line_channel_matches(
	const struct v34_phase3_profile *profile,
	const struct qam_channel *channel)
{
	struct qam_channel expected;

	return channel != NULL &&
	       v34_training_line_channel(
		       profile, channel->sample_rate, &expected) ==
		       V34_TRAINING_LINE_OK &&
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
	struct v34_preemphasis preemphasis = line->preemphasis;
	struct v34_phase3_s_sequence sequence;
	enum calibration_stage stage = CALIBRATE_SYMBOLS;
	long double sum_squares = 0.0L;
	double peak = 0.0;
	uint32_t expected_samples = calibration_sample_count(line);
	uint32_t generated_samples = 0;
	uint16_t tail_symbols = 0;
	uint8_t filter_tail_samples = 0;

	if (expected_samples == 0 ||
	    v34_phase3_s_sequence_init(&sequence) !=
		    V34_PHASE3_S_SEQUENCE_OK)
		return V34_TRAINING_LINE_INVALID_ARGUMENT;
	while (stage != CALIBRATION_COMPLETE) {
		struct qam_point point = { 0 };
		enum v34_phase3_s_section section;
		double carrier_sample;
		double filtered_sample;
		double magnitude;
		bool accepting = v34_pulse_modulator_needs_point(&carrier);
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
		if (v34_pulse_modulator_sample(
			    &carrier, accepting ? &point : NULL,
			    &carrier_sample, &accepted) !=
			    V34_PULSE_MODULATOR_OK ||
		    (sequence_symbol && !accepted) ||
		    v34_preemphasis_sample(
			    &preemphasis, carrier_sample, &filtered_sample) !=
			    V34_PREEMPHASIS_OK ||
		    !isfinite(filtered_sample))
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
		magnitude = fabs(filtered_sample);
		sum_squares += (long double)filtered_sample * filtered_sample;
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
	    !v34_phase3_profile_is_valid(&line->profile) ||
	    !v34_pulse_modulator_is_valid(&line->carrier) ||
	    !v34_training_line_channel_matches(
		    &line->profile, &line->carrier.channel) ||
	    !occupied_band_fits(
		    &line->profile, line->carrier.channel.sample_rate) ||
	    line->carrier.amplitude != INT16_MAX ||
	    !v34_preemphasis_is_valid(&line->preemphasis) ||
	    line->preemphasis.sample_rate !=
		    line->carrier.channel.sample_rate ||
	    line->preemphasis.symbol_rate_numerator !=
		    line->profile.symbol_rate_numerator ||
	    line->preemphasis.symbol_rate_denominator !=
		    line->profile.symbol_rate_denominator ||
	    line->preemphasis.filter_index !=
		    line->profile.preemphasis_filter_index ||
	    line->preemphasis.next_history !=
		    line->generated_samples % V34_PREEMPHASIS_TAPS ||
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
	result = v34_training_line_channel(profile, sample_rate, &channel);
	if (result != V34_TRAINING_LINE_OK)
		return result;
	if (!isfinite(initialized.target_rms) ||
	    initialized.target_rms <= 0.0 ||
	    v34_pulse_modulator_init(
		    &initialized.carrier, profile, &channel, INT16_MAX,
		    V34_TRAINING_MAXIMUM_POINT_POWER) !=
		    V34_PULSE_MODULATOR_OK ||
	    v34_preemphasis_init(
		    &initialized.preemphasis, sample_rate,
		    profile->symbol_rate_numerator,
		    profile->symbol_rate_denominator,
		    profile->preemphasis_filter_index) !=
		    V34_PREEMPHASIS_OK)
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
	       v34_pulse_modulator_needs_point(&line->carrier);
}

enum v34_training_line_result v34_training_line_sample(
	struct v34_training_line *line,
	const struct qam_point *point, int16_t *sample,
	bool *point_accepted)
{
	struct v34_training_line next;
	double carrier_sample;
	double filtered_sample;
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
		    &carrier_sample, &accepted) != V34_PULSE_MODULATOR_OK)
		return point != NULL ? V34_TRAINING_LINE_INVALID_ARGUMENT :
				       V34_TRAINING_LINE_NUMERIC_FAILURE;
	if (accepted != accepting ||
	    v34_preemphasis_sample(
		    &next.preemphasis, carrier_sample, &filtered_sample) !=
		    V34_PREEMPHASIS_OK)
		return V34_TRAINING_LINE_NUMERIC_FAILURE;
	scaled_sample = filtered_sample * next.output_gain;
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

const char *v34_training_line_result_name(
	enum v34_training_line_result result)
{
	switch (result) {
	case V34_TRAINING_LINE_OK:
		return "ok";
	case V34_TRAINING_LINE_INVALID_ARGUMENT:
		return "invalid argument";
	case V34_TRAINING_LINE_OUT_OF_RANGE:
		return "out of range";
	case V34_TRAINING_LINE_CLIPPING_RISK:
		return "clipping risk";
	case V34_TRAINING_LINE_NUMERIC_FAILURE:
		return "numeric failure";
	}
	return "unknown";
}
