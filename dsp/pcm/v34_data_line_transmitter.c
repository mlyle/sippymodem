// SPDX-License-Identifier: BSD-3-Clause

#include "v34_data_line_transmitter.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>

static const double V34_EQUAL_POWER_RMS =
	0.70710678118654752440084436210485;
static const double V34_LINE_POWER_TOLERANCE = 1e-9;

static enum v34_data_line_transmitter_result carrier_result(
	enum v34_data_carrier_result result)
{
	switch (result) {
	case V34_DATA_CARRIER_OK:
		return V34_DATA_LINE_TRANSMITTER_OK;
	case V34_DATA_CARRIER_MAPPING_FRAME_REQUIRED:
		return V34_DATA_LINE_TRANSMITTER_MAPPING_FRAME_REQUIRED;
	case V34_DATA_CARRIER_MAPPING_FRAME_PENDING:
		return V34_DATA_LINE_TRANSMITTER_MAPPING_FRAME_PENDING;
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

static bool power_metadata_is_valid(
	const struct v34_data_line_transmitter *transmitter)
{
	double reduction_gain;
	double expected_target;
	double calibrated_rms;
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
	scale = fmax(1.0, expected_target);
	return isfinite(expected_target) && isfinite(calibrated_rms) &&
	       fabs(transmitter->target_rms - expected_target) <=
		       V34_LINE_POWER_TOLERANCE * scale &&
	       fabs(transmitter->output_gain * calibrated_rms -
		    transmitter->target_rms) <=
		       V34_LINE_POWER_TOLERANCE * scale &&
	       transmitter->calibration.peak_amplitude *
		       transmitter->output_gain <
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
	double calibrated_rms;
	double reduction_gain;
	enum v34_power_calibration_result calibration_result;
	enum v34_preemphasis_result preemphasis_result;

	if (transmitter == NULL || nominal_peak_amplitude <= 0)
		return V34_DATA_LINE_TRANSMITTER_INVALID_ARGUMENT;
	calibration_result = v34_calibrate_line_power(
		calibration_schedule, calibration_scrambler,
		calibration_encoder, normalizer, profile, sample_rate,
		&initialized.calibration);
	if (calibration_result == V34_POWER_CALIBRATION_OUT_OF_RANGE)
		return V34_DATA_LINE_TRANSMITTER_OUT_OF_RANGE;
	if (calibration_result ==
	    V34_POWER_CALIBRATION_NUMERIC_FAILURE)
		return V34_DATA_LINE_TRANSMITTER_NUMERIC_FAILURE;
	if (calibration_result != V34_POWER_CALIBRATION_OK)
		return V34_DATA_LINE_TRANSMITTER_INVALID_ARGUMENT;
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
	initialized.nominal_peak_amplitude = nominal_peak_amplitude;
	reduction_gain = pow(
		10.0, -(double)profile->transmit_power_reduction_db / 20.0);
	initialized.target_rms = nominal_peak_amplitude * V34_EQUAL_POWER_RMS *
		reduction_gain;
	calibrated_rms =
		sqrt(initialized.calibration.mean_squared_amplitude);
	initialized.output_gain = initialized.target_rms / calibrated_rms;
	if (!isfinite(initialized.output_gain) ||
	    initialized.output_gain <= 0.0 ||
	    initialized.calibration.peak_amplitude *
		    initialized.output_gain >=
		    INT16_MAX + 0.5)
		return V34_DATA_LINE_TRANSMITTER_CLIPPING_RISK;
	if (!v34_data_line_transmitter_is_valid(&initialized))
		return V34_DATA_LINE_TRANSMITTER_NUMERIC_FAILURE;
	*transmitter = initialized;
	return V34_DATA_LINE_TRANSMITTER_OK;
}

bool v34_data_line_transmitter_needs_mapping_frame(
	const struct v34_data_line_transmitter *transmitter)
{
	return v34_data_line_transmitter_is_valid(transmitter) &&
	       v34_data_carrier_needs_mapping_frame(
		       &transmitter->carrier);
}

enum v34_data_line_transmitter_result
v34_data_line_transmitter_load_mapping_frame(
	struct v34_data_line_transmitter *transmitter,
	const struct v34_transmitted_mapping_frame *frame)
{
	if (!v34_data_line_transmitter_is_valid(transmitter))
		return V34_DATA_LINE_TRANSMITTER_INVALID_ARGUMENT;
	return carrier_result(v34_data_carrier_load_mapping_frame(
		&transmitter->carrier, frame));
}

enum v34_data_line_transmitter_result
v34_data_line_transmitter_sample(
	struct v34_data_line_transmitter *transmitter,
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
		&next.carrier, &carrier_sample, &accepted));
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
	case V34_DATA_LINE_TRANSMITTER_MAPPING_FRAME_REQUIRED:
		return "mapping frame required";
	case V34_DATA_LINE_TRANSMITTER_MAPPING_FRAME_PENDING:
		return "mapping frame pending";
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
