// SPDX-License-Identifier: BSD-3-Clause

#include "v34_receive_timing.h"

#include "integer_math.h"
#include "memory_region.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

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
		       v34_station_peer_role(
			       timing->transmitting_profile.local_role) &&
	       (!timing->reanchor_at_s || !timing->s_detector.detected) &&
	       (timing->s_detector.acquired ||
		timing->timing.mode == V34_TIMING_CONTROL_LOCKED) &&
	       queue_state_is_valid(timing);
}

enum v34_receive_timing_result v34_receive_timing_init(
	struct v34_receive_timing *timing,
	const struct v34_phase3_profile *transmitting_profile,
	int16_t initial_gain, bool reanchor_at_s)
{
	struct v34_receive_timing initialized;

	if (timing == NULL || initial_gain <= 0 ||
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
		    v34_station_peer_role(
			    transmitting_profile->local_role)) !=
		    V34_TIMING_CONTROL_OK ||
	    v34_gain_control_init(
		    &initialized.gain, initial_gain) !=
		    V34_GAIN_CONTROL_OK ||
	    v34_gain_control_set_mode(
		    &initialized.gain, V34_GAIN_FROZEN) !=
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
	initialized.reanchor_at_s = reanchor_at_s;
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

bool v34_receive_timing_matches_profile(
	const struct v34_receive_timing *timing,
	const struct v34_phase3_profile *transmitting_profile)
{
	return v34_receive_timing_is_valid(timing) &&
	       v34_phase3_profile_is_valid(transmitting_profile) &&
	       v34_phase3_profile_is_equal(
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

enum v34_receive_timing_result v34_receive_timing_set_data_frozen(
	struct v34_receive_timing *timing, bool frozen)
{
	struct v34_receive_timing next;

	if (!v34_receive_timing_is_valid(timing) ||
	    !timing->s_detector.acquired)
		return V34_RECEIVE_TIMING_INVALID_STATE;
	next = *timing;
	if (v34_timing_control_set_data_frozen(&next.timing, frozen) !=
		    V34_TIMING_CONTROL_OK ||
	    !v34_receive_timing_is_valid(&next))
		return V34_RECEIVE_TIMING_INVALID_STATE;
	*timing = next;
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

static int16_t interpolate(
	int16_t current, uint16_t phase, int16_t previous, uint16_t remaining)
{
	uint32_t value = (uint32_t)((int32_t)current * phase) +
			 (uint32_t)((int32_t)previous * remaining);

	return round_shift_s32_word_to_s16(value, 14);
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
			bool detector_was_detected = timing->s_detector.detected;
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
			/* Silence and echo must not train the line gain.  The
			 * carrier-specific S acquisition edge starts gain pull-in;
			 * later qualification still gates the semantic receive epoch. */
			if (!detector_was_acquired && timing->s_detector.acquired &&
			    v34_gain_control_set_mode(
				    &timing->gain, V34_GAIN_TRACKING) !=
				    V34_GAIN_CONTROL_OK)
				return V34_RECEIVE_TIMING_INVALID_STATE;
			if (!detector_was_detected && timing->s_detector.detected &&
			    timing->reanchor_at_s) {
				/* Discard the provisional carrier epoch without discarding
				 * the qualified S group that established this boundary. */
				if (v34_timing_control_begin_acquisition(
					    &timing->timing, true) !=
					    V34_TIMING_CONTROL_OK)
					return V34_RECEIVE_TIMING_INVALID_STATE;
				timing->reanchor_at_s = false;
			}
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
