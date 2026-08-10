// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_v34_data_line_transmitter.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>

static const double V34_EQUAL_POWER_RMS =
	0.70710678118654752440084436210485;
static const double V34_LINE_POWER_TOLERANCE = 1e-9;

static enum sippymodem_v34_data_line_transmitter_result carrier_result(
	enum sippymodem_v34_data_carrier_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_DATA_CARRIER_OK:
		return SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_OK;
	case SIPPYMODEM_V34_DATA_CARRIER_MAPPING_FRAME_REQUIRED:
		return SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_MAPPING_FRAME_REQUIRED;
	case SIPPYMODEM_V34_DATA_CARRIER_MAPPING_FRAME_PENDING:
		return SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_MAPPING_FRAME_PENDING;
	case SIPPYMODEM_V34_DATA_CARRIER_OUT_OF_RANGE:
		return SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_OUT_OF_RANGE;
	case SIPPYMODEM_V34_DATA_CARRIER_NUMERIC_FAILURE:
		return SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_NUMERIC_FAILURE;
	case SIPPYMODEM_V34_DATA_CARRIER_INVALID_ARGUMENT:
		break;
	}
	return SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_INVALID_ARGUMENT;
}

static bool filter_matches_carrier(
	const struct sippymodem_v34_preemphasis *filter,
	const struct sippymodem_v34_data_carrier *carrier)
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
	const struct sippymodem_v34_data_line_transmitter *transmitter)
{
	double reduction_gain;
	double expected_target;
	double calibrated_rms;
	double scale;

	if (transmitter->nominal_peak_amplitude <= 0 ||
	    !sippymodem_v34_line_power_measurement_is_valid(
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

bool sippymodem_v34_data_line_transmitter_is_valid(
	const struct sippymodem_v34_data_line_transmitter *transmitter)
{
	uint32_t expected_carrier_phase;
	uint64_t expected_symbol_phase;

	if (transmitter == NULL ||
	    !sippymodem_v34_data_carrier_is_valid(&transmitter->carrier) ||
	    !sippymodem_v34_preemphasis_is_valid(&transmitter->preemphasis) ||
	    !filter_matches_carrier(
		    &transmitter->preemphasis, &transmitter->carrier) ||
	    !power_metadata_is_valid(transmitter) ||
	    transmitter->preemphasis.next_history !=
		    transmitter->generated_samples %
			    SIPPYMODEM_V34_PREEMPHASIS_TAPS)
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

enum sippymodem_v34_data_line_transmitter_result
sippymodem_v34_data_line_transmitter_init(
	struct sippymodem_v34_data_line_transmitter *transmitter,
	const struct sippymodem_v34_frame_schedule *calibration_schedule,
	const struct sippymodem_v34_scrambler *calibration_scrambler,
	const struct sippymodem_v34_4d_encoder *calibration_encoder,
	const struct sippymodem_v34_power_normalizer *normalizer,
	const struct sippymodem_v34_phase3_profile *profile,
	uint32_t sample_rate, int16_t nominal_peak_amplitude)
{
	struct sippymodem_v34_data_line_transmitter initialized = { 0 };
	double calibrated_rms;
	double reduction_gain;
	enum sippymodem_v34_power_calibration_result calibration_result;
	enum sippymodem_v34_preemphasis_result preemphasis_result;

	if (transmitter == NULL || nominal_peak_amplitude <= 0)
		return SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_INVALID_ARGUMENT;
	calibration_result = sippymodem_v34_calibrate_line_power(
		calibration_schedule, calibration_scrambler,
		calibration_encoder, normalizer, profile, sample_rate,
		&initialized.calibration);
	if (calibration_result == SIPPYMODEM_V34_POWER_CALIBRATION_OUT_OF_RANGE)
		return SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_OUT_OF_RANGE;
	if (calibration_result ==
	    SIPPYMODEM_V34_POWER_CALIBRATION_NUMERIC_FAILURE)
		return SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_NUMERIC_FAILURE;
	if (calibration_result != SIPPYMODEM_V34_POWER_CALIBRATION_OK)
		return SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_INVALID_ARGUMENT;
	if (sippymodem_v34_data_carrier_init(
		    &initialized.carrier, profile,
		    &calibration_schedule->geometry, normalizer, sample_rate) !=
		    SIPPYMODEM_V34_DATA_CARRIER_OK)
		return SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_INVALID_ARGUMENT;
	preemphasis_result = sippymodem_v34_preemphasis_init(
		&initialized.preemphasis, sample_rate,
		profile->symbol_rate_numerator, profile->symbol_rate_denominator,
		profile->preemphasis_filter_index);
	if (preemphasis_result == SIPPYMODEM_V34_PREEMPHASIS_NUMERIC_FAILURE)
		return SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_NUMERIC_FAILURE;
	if (preemphasis_result != SIPPYMODEM_V34_PREEMPHASIS_OK)
		return SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_INVALID_ARGUMENT;
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
		return SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_CLIPPING_RISK;
	if (!sippymodem_v34_data_line_transmitter_is_valid(&initialized))
		return SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_NUMERIC_FAILURE;
	*transmitter = initialized;
	return SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_OK;
}

bool sippymodem_v34_data_line_transmitter_needs_mapping_frame(
	const struct sippymodem_v34_data_line_transmitter *transmitter)
{
	return sippymodem_v34_data_line_transmitter_is_valid(transmitter) &&
	       sippymodem_v34_data_carrier_needs_mapping_frame(
		       &transmitter->carrier);
}

enum sippymodem_v34_data_line_transmitter_result
sippymodem_v34_data_line_transmitter_load_mapping_frame(
	struct sippymodem_v34_data_line_transmitter *transmitter,
	const struct sippymodem_v34_transmitted_mapping_frame *frame)
{
	if (!sippymodem_v34_data_line_transmitter_is_valid(transmitter))
		return SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_INVALID_ARGUMENT;
	return carrier_result(sippymodem_v34_data_carrier_load_mapping_frame(
		&transmitter->carrier, frame));
}

enum sippymodem_v34_data_line_transmitter_result
sippymodem_v34_data_line_transmitter_sample(
	struct sippymodem_v34_data_line_transmitter *transmitter,
	int16_t *sample, bool *symbol_accepted)
{
	struct sippymodem_v34_data_line_transmitter next;
	double carrier_sample;
	double filtered_sample;
	double scaled_sample;
	long rounded;
	bool accepted;
	enum sippymodem_v34_data_line_transmitter_result result;

	if (!sippymodem_v34_data_line_transmitter_is_valid(transmitter) ||
	    sample == NULL || symbol_accepted == NULL)
		return SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_INVALID_ARGUMENT;
	next = *transmitter;
	result = carrier_result(sippymodem_v34_data_carrier_sample(
		&next.carrier, &carrier_sample, &accepted));
	if (result != SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_OK)
		return result;
	if (sippymodem_v34_preemphasis_sample(
		    &next.preemphasis, carrier_sample, &filtered_sample) !=
	    SIPPYMODEM_V34_PREEMPHASIS_OK)
		return SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_NUMERIC_FAILURE;
	scaled_sample = filtered_sample * next.output_gain;
	if (!isfinite(scaled_sample))
		return SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_NUMERIC_FAILURE;
	if (scaled_sample <= INT16_MIN - 0.5 ||
	    scaled_sample >= INT16_MAX + 0.5)
		return SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_CLIPPING_RISK;
	rounded = lround(scaled_sample);
	if (rounded < INT16_MIN || rounded > INT16_MAX)
		return SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_CLIPPING_RISK;
	if (next.generated_samples == UINT64_MAX)
		return SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_OUT_OF_RANGE;
	next.generated_samples++;
	if (!sippymodem_v34_data_line_transmitter_is_valid(&next))
		return SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_NUMERIC_FAILURE;
	*transmitter = next;
	*sample = (int16_t)rounded;
	*symbol_accepted = accepted;
	return SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_OK;
}

const char *sippymodem_v34_data_line_transmitter_result_name(
	enum sippymodem_v34_data_line_transmitter_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_OK:
		return "ok";
	case SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_MAPPING_FRAME_REQUIRED:
		return "mapping frame required";
	case SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_MAPPING_FRAME_PENDING:
		return "mapping frame pending";
	case SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_INVALID_ARGUMENT:
		return "invalid argument";
	case SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_OUT_OF_RANGE:
		return "out of range";
	case SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_CLIPPING_RISK:
		return "clipping risk";
	case SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_NUMERIC_FAILURE:
		return "numeric failure";
	}
	return "unknown";
}
