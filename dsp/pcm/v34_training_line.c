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

enum calibration_stage {
	CALIBRATE_SYMBOLS,
	CALIBRATE_PULSE_TAIL,
	CALIBRATE_FILTER_TAIL,
	CALIBRATION_COMPLETE,
};

static struct qam_channel training_channel(
	const struct v34_phase3_profile *profile,
	uint32_t sample_rate)
{
	return (struct qam_channel){
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
}

static bool channel_matches_profile(
	const struct qam_channel *channel,
	const struct v34_phase3_profile *profile)
{
	return channel != NULL && profile != NULL &&
	       channel->symbol_rate_numerator ==
		       profile->symbol_rate_numerator &&
	       channel->symbol_rate_denominator ==
		       profile->symbol_rate_denominator &&
	       channel->carrier_frequency_numerator ==
		       profile->carrier_frequency_numerator &&
	       channel->carrier_frequency_denominator ==
		       profile->carrier_frequency_denominator &&
	       channel->rolloff == V34_TRAINING_ROLLOFF &&
	       channel->pulse_span_symbols ==
		       V34_TRAINING_LINE_PULSE_SPAN_SYMBOLS;
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
		0.5L * (1.0L + V34_TRAINING_ROLLOFF) * symbol_rate;

	return upper_edge < 0.5L * sample_rate;
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
	struct qam_modulator carrier = line->carrier;
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
		const struct qam_point zero = { 0 };
		struct qam_point point = zero;
		enum v34_phase3_s_section section;
		double carrier_sample;
		double filtered_sample;
		double magnitude;
		bool accepting = qam_modulator_can_accept_symbol(&carrier);
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
		if (qam_modulator_sample_f64(
			    &carrier, &point, &carrier_sample, &accepted) !=
			    QAM_OK ||
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
	uint32_t expected_carrier_phase;
	uint64_t expected_symbol_phase;

	if (line == NULL ||
	    !v34_phase3_profile_is_valid(&line->profile) ||
	    !qam_modulator_is_valid(&line->carrier) ||
	    !channel_matches_profile(&line->carrier.channel, &line->profile) ||
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
	    !power_metadata_is_valid(line))
		return false;
	expected_carrier_phase = (uint32_t)line->generated_samples *
		line->carrier.carrier_phase_step;
	expected_symbol_phase =
		(line->generated_samples % line->carrier.symbol_period_units) *
		line->profile.symbol_rate_numerator %
		line->carrier.symbol_period_units;
	return line->carrier.carrier_phase == expected_carrier_phase &&
	       line->carrier.symbol_phase_units == expected_symbol_phase;
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
	if (!occupied_band_fits(profile, sample_rate))
		return V34_TRAINING_LINE_OUT_OF_RANGE;
	initialized.profile = *profile;
	initialized.nominal_peak_amplitude = nominal_peak_amplitude;
	reduction_gain = pow(
		10.0, -(double)profile->transmit_power_reduction_db / 20.0);
	initialized.target_rms = nominal_peak_amplitude *
		V34_EQUAL_POWER_RMS * reduction_gain;
	channel = training_channel(profile, sample_rate);
	if (!isfinite(initialized.target_rms) ||
	    initialized.target_rms <= 0.0 ||
	    qam_modulator_init(
		    &initialized.carrier, &channel, INT16_MAX) != QAM_OK ||
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
	       qam_modulator_can_accept_symbol(&line->carrier);
}

enum v34_training_line_result v34_training_line_sample(
	struct v34_training_line *line,
	const struct qam_point *point, int16_t *sample,
	bool *point_accepted)
{
	static const struct qam_point zero = { 0 };
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
	accepting = qam_modulator_can_accept_symbol(&line->carrier);
	if ((point != NULL) != accepting)
		return V34_TRAINING_LINE_INVALID_ARGUMENT;
	next = *line;
	if (qam_modulator_sample_f64(
		    &next.carrier, point != NULL ? point : &zero,
		    &carrier_sample, &accepted) != QAM_OK)
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
	if (next.generated_samples == UINT64_MAX)
		return V34_TRAINING_LINE_OUT_OF_RANGE;
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
