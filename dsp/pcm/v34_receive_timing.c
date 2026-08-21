// SPDX-License-Identifier: BSD-3-Clause

#include "v34_receive_timing.h"

#include "memory_region.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static enum v34_station_role receiving_role(
	enum v34_station_role transmitting_role)
{
	return transmitting_role == V34_CALLING_STATION ?
		       V34_ANSWERING_STATION : V34_CALLING_STATION;
}

static bool queue_state_is_valid(const struct v34_receive_timing *timing)
{
	return timing->input_read < V34_RECEIVE_TIMING_INPUT_CAPACITY &&
	       timing->input_count <= V34_RECEIVE_TIMING_INPUT_CAPACITY &&
	       timing->input_advance_pending <= 2u &&
	       timing->timing_output_count < V34_RECEIVE_TIMING_OUTPUTS &&
	       (!timing->current_valid ?
		(timing->input_advance_pending == 0u &&
		 timing->timing_output_count == 0u) : true);
}

bool v34_receive_timing_is_valid(
	const struct v34_receive_timing *timing)
{
	return timing != NULL && timing->initialized &&
	       v34_phase3_profile_is_valid(&timing->transmitting_profile) &&
	       v34_analytic_filter_is_valid(&timing->analytic) &&
	       v34_gain_control_is_valid(&timing->gain) &&
	       v34_s_tone_detector_is_valid(&timing->s_detector) &&
	       v34_carrier_demodulator_is_valid(&timing->carrier) &&
	       v34_timing_filter_is_valid(&timing->timing_filter) &&
	       v34_timing_control_is_valid(&timing->timing) &&
	       timing->timing.local_role ==
		       receiving_role(timing->transmitting_profile.local_role) &&
	       (timing->s_detector.acquired ?
		(timing->timing.mode != V34_TIMING_CONTROL_LOCKED) :
		(timing->timing.mode == V34_TIMING_CONTROL_LOCKED)) &&
	       queue_state_is_valid(timing);
}

enum v34_receive_timing_result v34_receive_timing_init(
	struct v34_receive_timing *timing,
	const struct v34_phase3_profile *transmitting_profile)
{
	struct v34_receive_timing initialized;

	if (timing == NULL ||
	    !v34_phase3_profile_is_valid(transmitting_profile))
		return V34_RECEIVE_TIMING_INVALID_ARGUMENT;
	memset(&initialized, 0, sizeof(initialized));
	initialized.transmitting_profile = *transmitting_profile;
	if (v34_analytic_filter_init(&initialized.analytic) !=
		    V34_ANALYTIC_FILTER_OK ||
	    v34_timing_control_init(
		    &initialized.timing, V34_RECEIVE_TIMING_SAMPLE_RATE,
		    transmitting_profile->symbol_rate_numerator,
		    transmitting_profile->symbol_rate_denominator,
		    receiving_role(transmitting_profile->local_role)) !=
		    V34_TIMING_CONTROL_OK ||
	    v34_gain_control_init(
		    &initialized.gain,
		    (int16_t)(initialized.timing.phase_limit / 2u)) !=
		    V34_GAIN_CONTROL_OK ||
	    v34_s_tone_detector_init(
		    &initialized.s_detector, transmitting_profile) !=
		    V34_S_TONE_DETECTOR_OK ||
	    v34_carrier_demodulator_init(
		    &initialized.carrier, V34_RECEIVE_TIMING_SAMPLE_RATE,
		    transmitting_profile->carrier_frequency_numerator,
		    transmitting_profile->carrier_frequency_denominator) !=
		    V34_CARRIER_DEMODULATOR_OK ||
	    v34_timing_filter_init(&initialized.timing_filter) !=
		    V34_TIMING_FILTER_OK)
		return V34_RECEIVE_TIMING_INVALID_ARGUMENT;
	initialized.initialized = true;
	if (!v34_receive_timing_is_valid(&initialized))
		return V34_RECEIVE_TIMING_INVALID_ARGUMENT;
	*timing = initialized;
	return V34_RECEIVE_TIMING_OK;
}

bool v34_receive_timing_is_acquired(
	const struct v34_receive_timing *timing)
{
	return v34_receive_timing_is_valid(timing) &&
	       timing->s_detector.detected &&
	       timing->timing.mode != V34_TIMING_CONTROL_INITIAL_PHASE;
}

static bool profiles_are_equal(
	const struct v34_phase3_profile *left,
	const struct v34_phase3_profile *right)
{
	return left->symbol_rate_numerator == right->symbol_rate_numerator &&
	       left->symbol_rate_denominator == right->symbol_rate_denominator &&
	       left->carrier_frequency_numerator ==
		       right->carrier_frequency_numerator &&
	       left->carrier_frequency_denominator ==
		       right->carrier_frequency_denominator &&
	       left->md_symbol_count == right->md_symbol_count &&
	       left->symbol_rate == right->symbol_rate &&
	       left->local_role == right->local_role &&
	       left->transmit_power_reduction_db ==
		       right->transmit_power_reduction_db &&
	       left->preemphasis_filter_index ==
		       right->preemphasis_filter_index &&
	       left->projected_data_rate_2400_bps_units ==
		       right->projected_data_rate_2400_bps_units &&
	       left->md_length_35_ms_units == right->md_length_35_ms_units &&
	       left->high_carrier == right->high_carrier;
}

bool v34_receive_timing_matches_profile(
	const struct v34_receive_timing *timing,
	const struct v34_phase3_profile *transmitting_profile)
{
	return v34_receive_timing_is_valid(timing) &&
	       v34_phase3_profile_is_valid(transmitting_profile) &&
	       profiles_are_equal(
		       &timing->transmitting_profile, transmitting_profile);
}

enum v34_receive_timing_result v34_receive_timing_freeze_gain(
	struct v34_receive_timing *timing)
{
	if (!v34_receive_timing_is_valid(timing))
		return V34_RECEIVE_TIMING_INVALID_STATE;
	if (v34_gain_control_set_mode(&timing->gain, V34_GAIN_FROZEN) !=
	    V34_GAIN_CONTROL_OK)
		return V34_RECEIVE_TIMING_INVALID_STATE;
	return V34_RECEIVE_TIMING_OK;
}

static struct v34_analytic_sample queue_pop(
	struct v34_receive_timing *timing)
{
	struct v34_analytic_sample sample = timing->input[timing->input_read];

	timing->input_read++;
	if (timing->input_read == V34_RECEIVE_TIMING_INPUT_CAPACITY)
		timing->input_read = 0;
	timing->input_count--;
	return sample;
}

static void queue_push(
	struct v34_receive_timing *timing, struct v34_analytic_sample sample)
{
	uint8_t write = (uint8_t)(timing->input_read + timing->input_count);

	if (write >= V34_RECEIVE_TIMING_INPUT_CAPACITY)
		write = (uint8_t)(write - V34_RECEIVE_TIMING_INPUT_CAPACITY);
	timing->input[write] = sample;
	timing->input_count++;
}

static bool advance_input(struct v34_receive_timing *timing)
{
	while (timing->input_advance_pending != 0u) {
		if (timing->input_count == 0u)
			return false;
		timing->previous = timing->current;
		timing->current = queue_pop(timing);
		timing->input_advance_pending--;
	}
	if (!timing->current_valid) {
		if (timing->input_count == 0u)
			return false;
		timing->current = queue_pop(timing);
		timing->current_valid = true;
	}
	return true;
}

static int32_t arithmetic_shift_right(uint32_t value, unsigned int shift)
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

static int16_t wrap_s16(int32_t value)
{
	uint16_t word = (uint16_t)(uint32_t)value;

	return word <= INT16_MAX ?
		       (int16_t)word : (int16_t)((int32_t)word - 65536);
}

static int16_t interpolate(
	int16_t current, uint16_t phase, int16_t previous, uint16_t remaining)
{
	uint32_t value = (uint32_t)((int32_t)current * phase) +
			 UINT32_C(0x2000) +
			 (uint32_t)((int32_t)previous * remaining);

	return wrap_s16(arithmetic_shift_right(value, 14));
}

static struct v34_analytic_sample interpolate_current(
	const struct v34_receive_timing *timing)
{
	uint16_t phase = timing->timing.phase;
	uint16_t remaining = (uint16_t)(timing->timing.phase_limit - phase);

	return (struct v34_analytic_sample){
		.in_phase = interpolate(
			timing->current.in_phase, phase,
			timing->previous.in_phase, remaining),
		.quadrature = interpolate(
			timing->current.quadrature, phase,
			timing->previous.quadrature, remaining),
	};
}

static void advance_phase(struct v34_receive_timing *timing)
{
	uint32_t next = (uint32_t)timing->timing.phase +
			timing->timing.phase_increment;
	uint16_t advances = 0;

	while (next >= timing->timing.phase_limit) {
		next -= timing->timing.phase_limit;
		advances++;
	}
	timing->timing.phase = (uint16_t)next;
	timing->input_advance_pending = (uint8_t)advances;
}

static enum v34_receive_timing_result drain_timing(
	struct v34_receive_timing *timing,
	struct v34_receive_timing_samples *samples)
{
	while (advance_input(timing)) {
		struct v34_analytic_sample interpolated =
			interpolate_current(timing);
		int16_t timing_error;
		uint8_t output_index = timing->timing_output_count;

		if (v34_timing_filter_observe(
			    &timing->timing_filter, interpolated,
			    &timing_error) != V34_TIMING_FILTER_OK)
			return V34_RECEIVE_TIMING_INVALID_STATE;
		timing->timing_observations[output_index + 1u] = timing_error;
		if ((output_index & 1u) != 0u) {
			if (v34_timing_filter_prefilter(
				    &timing->timing_filter,
				    &samples->sample[samples->count]) !=
				    V34_TIMING_FILTER_OK)
				return V34_RECEIVE_TIMING_INVALID_STATE;
			samples->count++;
		}
		timing->timing_output_count++;
		advance_phase(timing);
		if (timing->timing_output_count == V34_RECEIVE_TIMING_OUTPUTS) {
			enum v34_s_tone_detector_result detector_result;
			bool detector_was_acquired = timing->s_detector.acquired;
			uint16_t increment;

			detector_result = v34_s_tone_detector_end_group(
				&timing->s_detector);
			if (detector_result != V34_S_TONE_DETECTOR_OK &&
			    detector_result != V34_S_TONE_DETECTOR_DETECTED)
				return V34_RECEIVE_TIMING_INVALID_STATE;
			/* Begin clock pull-in at the detector's lower carrier-acquired
			 * boundary.  The later qualified result gates public readiness,
			 * leaving the loop several S intervals to establish its phase. */
			if (!detector_was_acquired && timing->s_detector.acquired &&
			    timing->timing.mode == V34_TIMING_CONTROL_LOCKED &&
			    v34_timing_control_begin_acquisition(
				    &timing->timing, true) != V34_TIMING_CONTROL_OK)
				return V34_RECEIVE_TIMING_INVALID_STATE;
			if (v34_timing_control_process(
				    &timing->timing,
				    timing->timing_observations,
				    &increment) != V34_TIMING_CONTROL_OK ||
			    increment != timing->timing.phase_increment)
				return V34_RECEIVE_TIMING_INVALID_STATE;
			timing->timing_output_count = 0;
			samples->symbol_complete = true;
			return V34_RECEIVE_TIMING_OK;
		}
	}
	return V34_RECEIVE_TIMING_OK;
}

enum v34_receive_timing_result v34_receive_timing_sample(
	struct v34_receive_timing *timing, int16_t sample,
	struct v34_receive_timing_samples *samples)
{
	struct v34_receive_timing_samples generated = { 0 };
	struct v34_analytic_sample analytic;
	struct v34_analytic_sample gained;
	struct v34_analytic_sample demodulated;
	enum v34_s_tone_detector_result detector_result;
	enum v34_receive_timing_result result;

	if (!v34_receive_timing_is_valid(timing))
		return V34_RECEIVE_TIMING_INVALID_STATE;
	if (samples == NULL ||
	    memory_regions_overlap(
		    timing, sizeof(*timing), samples, sizeof(*samples)))
		return V34_RECEIVE_TIMING_INVALID_ARGUMENT;
	if (timing->input_count == V34_RECEIVE_TIMING_INPUT_CAPACITY)
		return V34_RECEIVE_TIMING_INVALID_STATE;
	if (v34_analytic_filter_process(
		    &timing->analytic, sample, &analytic) !=
		    V34_ANALYTIC_FILTER_OK ||
	    v34_gain_control_process(&timing->gain, analytic, &gained) !=
		    V34_GAIN_CONTROL_OK)
		return V34_RECEIVE_TIMING_INVALID_STATE;
	detector_result = v34_s_tone_detector_sample(
		&timing->s_detector, gained.in_phase);
	if (detector_result != V34_S_TONE_DETECTOR_OK &&
	    detector_result != V34_S_TONE_DETECTOR_DETECTED)
		return V34_RECEIVE_TIMING_INVALID_STATE;
	if (v34_carrier_demodulator_process(
		    &timing->carrier, gained, &demodulated) !=
		    V34_CARRIER_DEMODULATOR_OK)
		return V34_RECEIVE_TIMING_INVALID_STATE;
	queue_push(timing, demodulated);
	result = drain_timing(timing, &generated);
	if (result != V34_RECEIVE_TIMING_OK)
		return result;
	if (!v34_receive_timing_is_valid(timing))
		return V34_RECEIVE_TIMING_INVALID_STATE;
	if (generated.count > V34_RECEIVE_TIMING_INTERVAL_SAMPLES ||
	    (generated.symbol_complete && generated.count == 0u) ||
	    (!generated.symbol_complete &&
	     generated.count == V34_RECEIVE_TIMING_INTERVAL_SAMPLES))
		return V34_RECEIVE_TIMING_INVALID_STATE;
	if (generated.count == 0u)
		return V34_RECEIVE_TIMING_OK;
	*samples = generated;
	return V34_RECEIVE_TIMING_SAMPLES_READY;
}
