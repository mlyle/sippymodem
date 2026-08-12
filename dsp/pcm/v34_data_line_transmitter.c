// SPDX-License-Identifier: BSD-3-Clause

#include "v34_data_line_transmitter.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>

static const double V34_EQUAL_POWER_RMS =
	0.70710678118654752440084436210485;
static const double V34_LINE_POWER_TOLERANCE = 1e-9;

struct line_power {
	struct v34_line_power_measurement calibration;
	double target_rms;
	double data_output_gain;
};

static enum v34_data_line_transmitter_result carrier_result(
	enum v34_data_carrier_result result)
{
	switch (result) {
	case V34_DATA_CARRIER_OK:
		return V34_DATA_LINE_TRANSMITTER_OK;
	case V34_DATA_CARRIER_OUT_OF_RANGE:
		return V34_DATA_LINE_TRANSMITTER_OUT_OF_RANGE;
	case V34_DATA_CARRIER_NUMERIC_FAILURE:
		return V34_DATA_LINE_TRANSMITTER_NUMERIC_FAILURE;
	case V34_DATA_CARRIER_INVALID_ARGUMENT:
		break;
	}
	return V34_DATA_LINE_TRANSMITTER_INVALID_ARGUMENT;
}

static bool filter_matches_carrier(
	const struct v34_preemphasis *filter,
	const struct v34_data_carrier *carrier)
{
	return filter->sample_rate == carrier->modulator.channel.sample_rate &&
	       filter->symbol_rate_numerator ==
		       carrier->profile.symbol_rate_numerator &&
	       filter->symbol_rate_denominator ==
		       carrier->profile.symbol_rate_denominator &&
	       filter->filter_index ==
		       carrier->profile.preemphasis_filter_index;
}

static enum v34_data_line_transmitter_result calibration_result(
	enum v34_power_calibration_result result)
{
	switch (result) {
	case V34_POWER_CALIBRATION_OK:
		return V34_DATA_LINE_TRANSMITTER_OK;
	case V34_POWER_CALIBRATION_OUT_OF_RANGE:
		return V34_DATA_LINE_TRANSMITTER_OUT_OF_RANGE;
	case V34_POWER_CALIBRATION_NUMERIC_FAILURE:
		return V34_DATA_LINE_TRANSMITTER_NUMERIC_FAILURE;
	case V34_POWER_CALIBRATION_INVALID_ARGUMENT:
		break;
	}
	return V34_DATA_LINE_TRANSMITTER_INVALID_ARGUMENT;
}

static enum v34_data_line_transmitter_result derive_line_power(
	const struct v34_frame_schedule *calibration_schedule,
	const struct v34_scrambler *calibration_scrambler,
	const struct v34_4d_encoder *calibration_encoder,
	const struct v34_power_normalizer *normalizer,
	const struct v34_phase3_profile *profile,
	uint32_t sample_rate, int16_t nominal_peak_amplitude,
	struct line_power *power)
{
	struct line_power derived;
	double calibrated_rms;
	double reduction_gain;
	enum v34_data_line_transmitter_result result;

	if (nominal_peak_amplitude <= 0 || power == NULL)
		return V34_DATA_LINE_TRANSMITTER_INVALID_ARGUMENT;
	result = calibration_result(v34_calibrate_line_power(
		calibration_schedule, calibration_scrambler,
		calibration_encoder, normalizer, profile, sample_rate,
		&derived.calibration));
	if (result != V34_DATA_LINE_TRANSMITTER_OK)
		return result;
	reduction_gain = pow(
		10.0, -(double)profile->transmit_power_reduction_db / 20.0);
	derived.target_rms = nominal_peak_amplitude * V34_EQUAL_POWER_RMS *
		reduction_gain;
	calibrated_rms = sqrt(derived.calibration.mean_squared_amplitude);
	derived.data_output_gain = derived.target_rms / calibrated_rms;
	if (!isfinite(derived.target_rms) || derived.target_rms <= 0.0 ||
	    !isfinite(derived.data_output_gain) ||
	    derived.data_output_gain <= 0.0)
		return V34_DATA_LINE_TRANSMITTER_NUMERIC_FAILURE;
	if (derived.calibration.peak_amplitude *
		    derived.data_output_gain >=
	    INT16_MAX + 0.5)
		return V34_DATA_LINE_TRANSMITTER_CLIPPING_RISK;
	*power = derived;
	return V34_DATA_LINE_TRANSMITTER_OK;
}

static bool power_metadata_is_valid(
	const struct v34_data_line_transmitter *transmitter)
{
	double reduction_gain;
	double expected_target;
	double calibrated_rms;
	double effective_data_gain;
	double scale;

	if (transmitter->nominal_peak_amplitude <= 0 ||
	    !v34_line_power_measurement_is_valid(
		    &transmitter->calibration) ||
	    !isfinite(transmitter->output_gain) ||
	    transmitter->output_gain <= 0.0 ||
	    !isfinite(transmitter->target_rms) ||
	    transmitter->target_rms <= 0.0)
		return false;
	reduction_gain = pow(
		10.0,
		-(double)transmitter->carrier.profile
			 .transmit_power_reduction_db /
			20.0);
	expected_target = transmitter->nominal_peak_amplitude *
		V34_EQUAL_POWER_RMS * reduction_gain;
	calibrated_rms =
		sqrt(transmitter->calibration.mean_squared_amplitude);
	effective_data_gain = transmitter->output_gain *
		transmitter->carrier.point_gain;
	scale = fmax(1.0, expected_target);
	return isfinite(expected_target) && isfinite(calibrated_rms) &&
	       isfinite(effective_data_gain) && effective_data_gain > 0.0 &&
	       fabs(transmitter->target_rms - expected_target) <=
		       V34_LINE_POWER_TOLERANCE * scale &&
	       fabs(effective_data_gain * calibrated_rms -
		    transmitter->target_rms) <=
		       V34_LINE_POWER_TOLERANCE * scale &&
	       transmitter->calibration.peak_amplitude *
		       effective_data_gain <
		       INT16_MAX + 0.5;
}

bool v34_data_line_transmitter_is_valid(
	const struct v34_data_line_transmitter *transmitter)
{
	uint32_t expected_carrier_phase;
	uint64_t expected_symbol_phase;

	if (transmitter == NULL ||
	    !v34_data_carrier_is_valid(&transmitter->carrier) ||
	    !v34_preemphasis_is_valid(&transmitter->preemphasis) ||
	    !filter_matches_carrier(
		    &transmitter->preemphasis, &transmitter->carrier) ||
	    !power_metadata_is_valid(transmitter) ||
	    transmitter->preemphasis.next_history !=
		    transmitter->generated_samples %
			    V34_PREEMPHASIS_TAPS)
		return false;
	expected_carrier_phase =
		(uint32_t)transmitter->generated_samples *
		transmitter->carrier.modulator.carrier_phase_step;
	expected_symbol_phase =
		(transmitter->generated_samples %
		 transmitter->carrier.modulator.symbol_period_units) *
		transmitter->carrier.profile.symbol_rate_numerator %
		transmitter->carrier.modulator.symbol_period_units;
	return transmitter->carrier.modulator.carrier_phase ==
		       expected_carrier_phase &&
	       transmitter->carrier.modulator.symbol_phase_units ==
		       expected_symbol_phase;
}

enum v34_data_line_transmitter_result
v34_data_line_transmitter_init(
	struct v34_data_line_transmitter *transmitter,
	const struct v34_frame_schedule *calibration_schedule,
	const struct v34_scrambler *calibration_scrambler,
	const struct v34_4d_encoder *calibration_encoder,
	const struct v34_power_normalizer *normalizer,
	const struct v34_phase3_profile *profile,
	uint32_t sample_rate, int16_t nominal_peak_amplitude)
{
	struct v34_data_line_transmitter initialized = { 0 };
	struct line_power power;
	enum v34_data_line_transmitter_result result;
	enum v34_preemphasis_result preemphasis_result;

	if (transmitter == NULL)
		return V34_DATA_LINE_TRANSMITTER_INVALID_ARGUMENT;
	result = derive_line_power(
		calibration_schedule, calibration_scrambler,
		calibration_encoder, normalizer, profile, sample_rate,
		nominal_peak_amplitude, &power);
	if (result != V34_DATA_LINE_TRANSMITTER_OK)
		return result;
	if (v34_data_carrier_init(
		    &initialized.carrier, profile,
		    &calibration_schedule->geometry, normalizer, sample_rate) !=
		    V34_DATA_CARRIER_OK)
		return V34_DATA_LINE_TRANSMITTER_INVALID_ARGUMENT;
	preemphasis_result = v34_preemphasis_init(
		&initialized.preemphasis, sample_rate,
		profile->symbol_rate_numerator, profile->symbol_rate_denominator,
		profile->preemphasis_filter_index);
	if (preemphasis_result == V34_PREEMPHASIS_NUMERIC_FAILURE)
		return V34_DATA_LINE_TRANSMITTER_NUMERIC_FAILURE;
	if (preemphasis_result != V34_PREEMPHASIS_OK)
		return V34_DATA_LINE_TRANSMITTER_INVALID_ARGUMENT;
	initialized.calibration = power.calibration;
	initialized.nominal_peak_amplitude = nominal_peak_amplitude;
	initialized.target_rms = power.target_rms;
	initialized.output_gain = power.data_output_gain;
	if (!v34_data_line_transmitter_is_valid(&initialized))
		return V34_DATA_LINE_TRANSMITTER_NUMERIC_FAILURE;
	*transmitter = initialized;
	return V34_DATA_LINE_TRANSMITTER_OK;
}

enum v34_data_line_transmitter_result
v34_data_line_transmitter_continue(
	struct v34_data_line_transmitter *transmitter,
	const struct v34_training_line *active_line,
	const struct v34_frame_schedule *calibration_schedule,
	const struct v34_scrambler *calibration_scrambler,
	const struct v34_4d_encoder *calibration_encoder,
	const struct v34_power_normalizer *normalizer)
{
	struct v34_data_line_transmitter continued = { 0 };
	struct line_power power;
	double point_gain;
	enum v34_data_line_transmitter_result result;

	if (transmitter == NULL || !v34_training_line_is_valid(active_line))
		return V34_DATA_LINE_TRANSMITTER_INVALID_ARGUMENT;
	result = derive_line_power(
		calibration_schedule, calibration_scrambler,
		calibration_encoder, normalizer, &active_line->profile,
		active_line->carrier.channel.sample_rate,
		active_line->nominal_peak_amplitude, &power);
	if (result != V34_DATA_LINE_TRANSMITTER_OK)
		return result;
	if (power.target_rms != active_line->target_rms)
		return V34_DATA_LINE_TRANSMITTER_NUMERIC_FAILURE;
	point_gain = power.data_output_gain / active_line->output_gain;
	result = carrier_result(v34_data_carrier_continue(
		&continued.carrier, &active_line->carrier,
		&active_line->profile, &calibration_schedule->geometry,
		normalizer, point_gain));
	if (result != V34_DATA_LINE_TRANSMITTER_OK)
		return result;
	continued.preemphasis = active_line->preemphasis;
	continued.calibration = power.calibration;
	continued.output_gain = active_line->output_gain;
	continued.target_rms = power.target_rms;
	continued.generated_samples = active_line->generated_samples;
	continued.nominal_peak_amplitude =
		active_line->nominal_peak_amplitude;
	if (!v34_data_line_transmitter_is_valid(&continued))
		return V34_DATA_LINE_TRANSMITTER_NUMERIC_FAILURE;
	*transmitter = continued;
	return V34_DATA_LINE_TRANSMITTER_OK;
}

bool v34_data_line_transmitter_needs_point(
	const struct v34_data_line_transmitter *transmitter)
{
	return v34_data_line_transmitter_is_valid(transmitter) &&
	       v34_data_carrier_needs_point(
		       &transmitter->carrier);
}

enum v34_data_line_transmitter_result
v34_data_line_transmitter_sample(
	struct v34_data_line_transmitter *transmitter,
	const struct qam_point *point,
	int16_t *sample, bool *symbol_accepted)
{
	struct v34_data_line_transmitter next;
	double carrier_sample;
	double filtered_sample;
	double scaled_sample;
	long rounded;
	bool accepted;
	enum v34_data_line_transmitter_result result;

	if (!v34_data_line_transmitter_is_valid(transmitter) ||
	    sample == NULL || symbol_accepted == NULL)
		return V34_DATA_LINE_TRANSMITTER_INVALID_ARGUMENT;
	next = *transmitter;
	result = carrier_result(v34_data_carrier_sample(
		&next.carrier, point, &carrier_sample, &accepted));
	if (result != V34_DATA_LINE_TRANSMITTER_OK)
		return result;
	if (v34_preemphasis_sample(
		    &next.preemphasis, carrier_sample, &filtered_sample) !=
	    V34_PREEMPHASIS_OK)
		return V34_DATA_LINE_TRANSMITTER_NUMERIC_FAILURE;
	scaled_sample = filtered_sample * next.output_gain;
	if (!isfinite(scaled_sample))
		return V34_DATA_LINE_TRANSMITTER_NUMERIC_FAILURE;
	if (scaled_sample <= INT16_MIN - 0.5 ||
	    scaled_sample >= INT16_MAX + 0.5)
		return V34_DATA_LINE_TRANSMITTER_CLIPPING_RISK;
	rounded = lround(scaled_sample);
	if (rounded < INT16_MIN || rounded > INT16_MAX)
		return V34_DATA_LINE_TRANSMITTER_CLIPPING_RISK;
	if (next.generated_samples == UINT64_MAX)
		return V34_DATA_LINE_TRANSMITTER_OUT_OF_RANGE;
	next.generated_samples++;
	if (!v34_data_line_transmitter_is_valid(&next))
		return V34_DATA_LINE_TRANSMITTER_NUMERIC_FAILURE;
	*transmitter = next;
	*sample = (int16_t)rounded;
	*symbol_accepted = accepted;
	return V34_DATA_LINE_TRANSMITTER_OK;
}

const char *v34_data_line_transmitter_result_name(
	enum v34_data_line_transmitter_result result)
{
	switch (result) {
	case V34_DATA_LINE_TRANSMITTER_OK:
		return "ok";
	case V34_DATA_LINE_TRANSMITTER_INVALID_ARGUMENT:
		return "invalid argument";
	case V34_DATA_LINE_TRANSMITTER_OUT_OF_RANGE:
		return "out of range";
	case V34_DATA_LINE_TRANSMITTER_CLIPPING_RISK:
		return "clipping risk";
	case V34_DATA_LINE_TRANSMITTER_NUMERIC_FAILURE:
		return "numeric failure";
	}
	return "unknown";
}
