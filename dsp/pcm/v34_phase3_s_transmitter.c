// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase3_s_transmitter.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>

static const double V34_PHASE3_S_ROLLOFF = 0.1;
static const double V34_PHASE3_S_EQUAL_POWER_RMS =
	0.70710678118654752440084436210485;
static const double V34_PHASE3_S_POWER_TOLERANCE = 1e-9;

static struct qam_channel marker_channel(
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
		.rolloff = V34_PHASE3_S_ROLLOFF,
		.pulse_span_symbols =
			V34_PHASE3_S_PULSE_SPAN_SYMBOLS,
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
	       channel->rolloff == V34_PHASE3_S_ROLLOFF &&
	       channel->pulse_span_symbols ==
		       V34_PHASE3_S_PULSE_SPAN_SYMBOLS;
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
	long double upper_edge =
		carrier + 0.5L * (1.0L + V34_PHASE3_S_ROLLOFF) * symbol_rate;

	return upper_edge < 0.5L * sample_rate;
}

static uint32_t expected_sample_count(
	const struct v34_phase3_s_transmitter *transmitter)
{
	uint64_t accepted_index =
		V34_PHASE3_S_SEQUENCE_SYMBOLS +
		V34_PHASE3_S_TAIL_SYMBOLS - 1u;
	uint64_t numerator = accepted_index *
		transmitter->carrier.channel.sample_rate *
		transmitter->profile.symbol_rate_denominator;
	uint64_t denominator = transmitter->profile.symbol_rate_numerator;
	uint64_t last_acceptance =
		(numerator + denominator - 1u) / denominator;
	uint64_t count = last_acceptance + 1u +
		V34_PHASE3_S_FILTER_TAIL_SAMPLES;

	return count <= UINT32_MAX ? (uint32_t)count : 0;
}

static bool stage_is_valid(
	const struct v34_phase3_s_transmitter *transmitter)
{
	bool sequence_complete =
		v34_phase3_s_sequence_is_complete(
			&transmitter->sequence);

	switch (transmitter->stage) {
	case V34_PHASE3_S_TRANSMIT_SYMBOLS:
		return transmitter->tail_symbols == 0 &&
		       transmitter->filter_tail_samples == 0;
	case V34_PHASE3_S_TRANSMIT_PULSE_TAIL:
		return sequence_complete && transmitter->tail_symbols > 0 &&
		       transmitter->tail_symbols <
			       V34_PHASE3_S_TAIL_SYMBOLS &&
		       transmitter->filter_tail_samples == 0;
	case V34_PHASE3_S_TRANSMIT_FILTER_TAIL:
		return sequence_complete &&
		       transmitter->tail_symbols ==
			       V34_PHASE3_S_TAIL_SYMBOLS &&
		       transmitter->filter_tail_samples <
			       V34_PHASE3_S_FILTER_TAIL_SAMPLES;
	case V34_PHASE3_S_TRANSMIT_COMPLETE:
		return sequence_complete &&
		       transmitter->tail_symbols ==
			       V34_PHASE3_S_TAIL_SYMBOLS &&
		       transmitter->filter_tail_samples ==
			       V34_PHASE3_S_FILTER_TAIL_SAMPLES;
	}
	return false;
}

static bool power_metadata_is_valid(
	const struct v34_phase3_s_transmitter *transmitter)
{
	double expected_target;
	double reduction_gain;
	double error_scale;

	if (transmitter->nominal_peak_amplitude <= 0 ||
	    !isfinite(transmitter->output_gain) ||
	    transmitter->output_gain <= 0.0 || !isfinite(transmitter->raw_rms) ||
	    transmitter->raw_rms <= 0.0 || !isfinite(transmitter->raw_peak) ||
	    transmitter->raw_peak <= 0.0 ||
	    !isfinite(transmitter->target_rms) ||
	    transmitter->target_rms <= 0.0)
		return false;
	reduction_gain = pow(
		10.0, -(double)transmitter->profile.transmit_power_reduction_db /
			      20.0);
	expected_target = transmitter->nominal_peak_amplitude *
		V34_PHASE3_S_EQUAL_POWER_RMS * reduction_gain;
	error_scale = fmax(1.0, expected_target);
	return fabs(transmitter->target_rms - expected_target) <=
		       V34_PHASE3_S_POWER_TOLERANCE * error_scale &&
	       fabs(transmitter->output_gain * transmitter->raw_rms -
		    transmitter->target_rms) <=
		       V34_PHASE3_S_POWER_TOLERANCE * error_scale &&
	       transmitter->raw_peak * transmitter->output_gain <=
		       INT16_MAX - 0.5;
}

bool v34_phase3_s_transmitter_is_valid(
	const struct v34_phase3_s_transmitter *transmitter)
{
	uint32_t expected_samples;

	if (transmitter == NULL ||
	    !v34_phase3_profile_is_valid(&transmitter->profile) ||
	    !qam_modulator_is_valid(&transmitter->carrier) ||
	    !channel_matches_profile(
		    &transmitter->carrier.channel, &transmitter->profile) ||
	    !occupied_band_fits(
		    &transmitter->profile,
		    transmitter->carrier.channel.sample_rate) ||
	    transmitter->carrier.amplitude != INT16_MAX ||
	    !v34_preemphasis_is_valid(
		    &transmitter->preemphasis) ||
	    transmitter->preemphasis.sample_rate !=
		    transmitter->carrier.channel.sample_rate ||
	    transmitter->preemphasis.symbol_rate_numerator !=
		    transmitter->profile.symbol_rate_numerator ||
	    transmitter->preemphasis.symbol_rate_denominator !=
		    transmitter->profile.symbol_rate_denominator ||
	    transmitter->preemphasis.filter_index !=
		    transmitter->profile.preemphasis_filter_index ||
	    !v34_phase3_s_sequence_is_valid(
		    &transmitter->sequence) ||
	    !stage_is_valid(transmitter) || !power_metadata_is_valid(transmitter))
		return false;
	expected_samples = expected_sample_count(transmitter);
	return expected_samples != 0 &&
	       transmitter->generated_samples <= expected_samples &&
	       (transmitter->stage !=
			V34_PHASE3_S_TRANSMIT_COMPLETE ||
		transmitter->generated_samples == expected_samples);
}

bool v34_phase3_s_transmitter_is_complete(
	const struct v34_phase3_s_transmitter *transmitter)
{
	return v34_phase3_s_transmitter_is_valid(transmitter) &&
	       transmitter->stage ==
		       V34_PHASE3_S_TRANSMIT_COMPLETE;
}

static enum v34_phase3_s_transmitter_result raw_sample(
	struct v34_phase3_s_transmitter *transmitter,
	double *sample)
{
	const struct qam_point zero = { 0 };
	struct qam_point point = zero;
	enum v34_phase3_s_section section;
	double carrier_sample;
	bool accepting = qam_modulator_can_accept_symbol(
		&transmitter->carrier);
	bool sequence_symbol = false;
	bool symbol_accepted;
	bool draining_filter =
		transmitter->stage ==
		V34_PHASE3_S_TRANSMIT_FILTER_TAIL;

	if (transmitter->stage ==
	    V34_PHASE3_S_TRANSMIT_COMPLETE)
		return V34_PHASE3_S_TRANSMITTER_COMPLETE;
	if (transmitter->stage ==
		    V34_PHASE3_S_TRANSMIT_SYMBOLS &&
	    accepting) {
		if (!v34_phase3_s_sequence_is_complete(
			    &transmitter->sequence)) {
			if (v34_phase3_s_sequence_next(
				    &transmitter->sequence, &point, &section) !=
			    V34_PHASE3_S_SEQUENCE_OK)
				return V34_PHASE3_S_TRANSMITTER_NUMERIC_FAILURE;
			sequence_symbol = true;
		} else {
			transmitter->stage =
				V34_PHASE3_S_TRANSMIT_PULSE_TAIL;
		}
	}
	if (qam_modulator_sample_f64(
		    &transmitter->carrier, &point, &carrier_sample,
		    &symbol_accepted) != QAM_OK ||
	    (sequence_symbol && !symbol_accepted) ||
	    v34_preemphasis_sample(
		    &transmitter->preemphasis, carrier_sample, sample) !=
		    V34_PREEMPHASIS_OK)
		return V34_PHASE3_S_TRANSMITTER_NUMERIC_FAILURE;
	if (symbol_accepted &&
	    transmitter->stage ==
		    V34_PHASE3_S_TRANSMIT_PULSE_TAIL) {
		transmitter->tail_symbols++;
		if (transmitter->tail_symbols ==
		    V34_PHASE3_S_TAIL_SYMBOLS)
			transmitter->stage =
				V34_PHASE3_S_TRANSMIT_FILTER_TAIL;
	}
	if (draining_filter) {
		transmitter->filter_tail_samples++;
		if (transmitter->filter_tail_samples ==
		    V34_PHASE3_S_FILTER_TAIL_SAMPLES)
			transmitter->stage =
				V34_PHASE3_S_TRANSMIT_COMPLETE;
	}
	transmitter->generated_samples++;
	return V34_PHASE3_S_TRANSMITTER_OK;
}

static enum v34_phase3_s_transmitter_result calibrate_power(
	struct v34_phase3_s_transmitter *transmitter)
{
	struct v34_phase3_s_transmitter calibration = *transmitter;
	double sum_squares = 0.0;
	double peak = 0.0;
	uint32_t expected_samples = expected_sample_count(transmitter);

	if (expected_samples == 0)
		return V34_PHASE3_S_TRANSMITTER_INVALID_ARGUMENT;
	while (calibration.stage !=
	       V34_PHASE3_S_TRANSMIT_COMPLETE) {
		double sample;
		double magnitude;

		if (calibration.generated_samples >= expected_samples ||
		    raw_sample(&calibration, &sample) !=
			    V34_PHASE3_S_TRANSMITTER_OK ||
		    !isfinite(sample))
			return V34_PHASE3_S_TRANSMITTER_NUMERIC_FAILURE;
		sum_squares += sample * sample;
		magnitude = fabs(sample);
		if (!isfinite(sum_squares))
			return V34_PHASE3_S_TRANSMITTER_NUMERIC_FAILURE;
		if (magnitude > peak)
			peak = magnitude;
	}
	if (calibration.generated_samples != expected_samples || peak <= 0.0 ||
	    sum_squares <= 0.0)
		return V34_PHASE3_S_TRANSMITTER_NUMERIC_FAILURE;
	transmitter->raw_rms = sqrt(sum_squares / expected_samples);
	transmitter->raw_peak = peak;
	transmitter->output_gain =
		transmitter->target_rms / transmitter->raw_rms;
	if (!isfinite(transmitter->raw_rms) || transmitter->raw_rms <= 0.0 ||
	    !isfinite(transmitter->output_gain) ||
	    transmitter->output_gain <= 0.0)
		return V34_PHASE3_S_TRANSMITTER_NUMERIC_FAILURE;
	if (transmitter->raw_peak * transmitter->output_gain >
	    INT16_MAX - 0.5)
		return V34_PHASE3_S_TRANSMITTER_CLIPPING_RISK;
	return V34_PHASE3_S_TRANSMITTER_OK;
}

enum v34_phase3_s_transmitter_result
v34_phase3_s_transmitter_init(
	struct v34_phase3_s_transmitter *transmitter,
	const struct v34_phase3_profile *profile,
	uint32_t sample_rate, int16_t nominal_peak_amplitude)
{
	struct v34_phase3_s_transmitter initialized = { 0 };
	struct qam_channel channel;
	double reduction_gain;
	enum v34_phase3_s_transmitter_result result;

	if (transmitter == NULL ||
	    !v34_phase3_profile_is_valid(profile) ||
	    sample_rate == 0 || nominal_peak_amplitude <= 0 ||
	    !occupied_band_fits(profile, sample_rate))
		return V34_PHASE3_S_TRANSMITTER_INVALID_ARGUMENT;
	initialized.profile = *profile;
	initialized.nominal_peak_amplitude = nominal_peak_amplitude;
	initialized.stage = V34_PHASE3_S_TRANSMIT_SYMBOLS;
	reduction_gain = pow(
		10.0, -(double)profile->transmit_power_reduction_db / 20.0);
	initialized.target_rms = nominal_peak_amplitude *
		V34_PHASE3_S_EQUAL_POWER_RMS * reduction_gain;
	channel = marker_channel(profile, sample_rate);
	if (!isfinite(initialized.target_rms) || initialized.target_rms <= 0.0 ||
	    qam_modulator_init(
		    &initialized.carrier, &channel, INT16_MAX) !=
		    QAM_OK ||
	    v34_preemphasis_init(
		    &initialized.preemphasis, sample_rate,
		    profile->symbol_rate_numerator,
		    profile->symbol_rate_denominator,
		    profile->preemphasis_filter_index) !=
		    V34_PREEMPHASIS_OK ||
	    v34_phase3_s_sequence_init(&initialized.sequence) !=
		    V34_PHASE3_S_SEQUENCE_OK)
		return V34_PHASE3_S_TRANSMITTER_INVALID_ARGUMENT;
	result = calibrate_power(&initialized);
	if (result != V34_PHASE3_S_TRANSMITTER_OK)
		return result;
	if (!v34_phase3_s_transmitter_is_valid(&initialized))
		return V34_PHASE3_S_TRANSMITTER_NUMERIC_FAILURE;
	*transmitter = initialized;
	return V34_PHASE3_S_TRANSMITTER_OK;
}

enum v34_phase3_s_transmitter_result
v34_phase3_s_transmitter_sample(
	struct v34_phase3_s_transmitter *transmitter,
	int16_t *sample)
{
	struct v34_phase3_s_transmitter next;
	double raw;
	double scaled;
	long rounded;

	if (!v34_phase3_s_transmitter_is_valid(transmitter) ||
	    sample == NULL)
		return V34_PHASE3_S_TRANSMITTER_INVALID_ARGUMENT;
	if (transmitter->stage ==
	    V34_PHASE3_S_TRANSMIT_COMPLETE)
		return V34_PHASE3_S_TRANSMITTER_COMPLETE;
	next = *transmitter;
	if (raw_sample(&next, &raw) !=
	    V34_PHASE3_S_TRANSMITTER_OK)
		return V34_PHASE3_S_TRANSMITTER_NUMERIC_FAILURE;
	scaled = raw * next.output_gain;
	if (!isfinite(scaled) || scaled < INT16_MIN - 0.499 ||
	    scaled > INT16_MAX + 0.499)
		return V34_PHASE3_S_TRANSMITTER_NUMERIC_FAILURE;
	rounded = lround(scaled);
	if (rounded < INT16_MIN || rounded > INT16_MAX ||
	    !v34_phase3_s_transmitter_is_valid(&next))
		return V34_PHASE3_S_TRANSMITTER_NUMERIC_FAILURE;
	*transmitter = next;
	*sample = (int16_t)rounded;
	return V34_PHASE3_S_TRANSMITTER_OK;
}

const char *v34_phase3_s_transmitter_result_name(
	enum v34_phase3_s_transmitter_result result)
{
	switch (result) {
	case V34_PHASE3_S_TRANSMITTER_OK:
		return "ok";
	case V34_PHASE3_S_TRANSMITTER_COMPLETE:
		return "complete";
	case V34_PHASE3_S_TRANSMITTER_INVALID_ARGUMENT:
		return "invalid argument";
	case V34_PHASE3_S_TRANSMITTER_CLIPPING_RISK:
		return "clipping risk";
	case V34_PHASE3_S_TRANSMITTER_NUMERIC_FAILURE:
		return "numeric failure";
	}
	return "unknown";
}
