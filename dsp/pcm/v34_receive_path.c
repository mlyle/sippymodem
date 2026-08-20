// SPDX-License-Identifier: BSD-3-Clause

#include "v34_receive_path.h"

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

static bool queue_state_is_valid(const struct v34_receive_path *path)
{
	return path->input_read < V34_RECEIVE_PATH_INPUT_CAPACITY &&
	       path->input_count <= V34_RECEIVE_PATH_INPUT_CAPACITY &&
	       path->input_advance_pending <= 2u &&
	       path->timing_output_count < V34_RECEIVE_PATH_TIMING_OUTPUTS &&
	       (!path->current_valid ?
		(path->input_advance_pending == 0u &&
		 path->timing_output_count == 0u) : true);
}

bool v34_receive_path_is_valid(const struct v34_receive_path *path)
{
	return path != NULL && path->initialized &&
	       v34_phase3_profile_is_valid(&path->transmitting_profile) &&
	       v34_analytic_filter_is_valid(&path->analytic) &&
	       v34_gain_control_is_valid(&path->gain) &&
	       v34_carrier_demodulator_is_valid(&path->carrier) &&
	       v34_timing_filter_is_valid(&path->timing_filter) &&
	       v34_timing_control_is_valid(&path->timing) &&
	       v34_equalizer_is_valid(&path->equalizer) &&
	       path->timing.local_role ==
		       receiving_role(path->transmitting_profile.local_role) &&
	       queue_state_is_valid(path);
}

enum v34_receive_path_result v34_receive_path_init(
	struct v34_receive_path *path,
	const struct v34_phase3_profile *transmitting_profile)
{
	struct v34_receive_path initialized;

	if (path == NULL ||
	    !v34_phase3_profile_is_valid(transmitting_profile))
		return V34_RECEIVE_PATH_INVALID_ARGUMENT;
	memset(&initialized, 0, sizeof(initialized));
	initialized.transmitting_profile = *transmitting_profile;
	if (v34_analytic_filter_init(&initialized.analytic) !=
		    V34_ANALYTIC_FILTER_OK ||
	    v34_timing_control_init(
		    &initialized.timing, V34_RECEIVE_PATH_SAMPLE_RATE,
		    transmitting_profile->symbol_rate_numerator,
		    transmitting_profile->symbol_rate_denominator,
		    receiving_role(transmitting_profile->local_role)) !=
		    V34_TIMING_CONTROL_OK ||
	    v34_gain_control_init(
		    &initialized.gain,
		    (int16_t)(initialized.timing.phase_limit / 2u)) !=
		    V34_GAIN_CONTROL_OK ||
	    v34_carrier_demodulator_init(
		    &initialized.carrier, V34_RECEIVE_PATH_SAMPLE_RATE,
		    transmitting_profile->carrier_frequency_numerator,
		    transmitting_profile->carrier_frequency_denominator) !=
		    V34_CARRIER_DEMODULATOR_OK ||
	    v34_timing_filter_init(&initialized.timing_filter) !=
		    V34_TIMING_FILTER_OK ||
	    v34_equalizer_init(&initialized.equalizer) != V34_EQUALIZER_OK ||
	    v34_timing_control_begin_acquisition(
		    &initialized.timing, true) != V34_TIMING_CONTROL_OK)
		return V34_RECEIVE_PATH_INVALID_ARGUMENT;
	initialized.initialized = true;
	if (!v34_receive_path_is_valid(&initialized))
		return V34_RECEIVE_PATH_INVALID_ARGUMENT;
	*path = initialized;
	return V34_RECEIVE_PATH_OK;
}

static struct v34_analytic_sample queue_pop(struct v34_receive_path *path)
{
	struct v34_analytic_sample sample = path->input[path->input_read];

	path->input_read++;
	if (path->input_read == V34_RECEIVE_PATH_INPUT_CAPACITY)
		path->input_read = 0;
	path->input_count--;
	return sample;
}

static void queue_push(
	struct v34_receive_path *path, struct v34_analytic_sample sample)
{
	uint8_t write = (uint8_t)(path->input_read + path->input_count);

	if (write >= V34_RECEIVE_PATH_INPUT_CAPACITY)
		write = (uint8_t)(write - V34_RECEIVE_PATH_INPUT_CAPACITY);
	path->input[write] = sample;
	path->input_count++;
}

static bool advance_input(struct v34_receive_path *path)
{
	while (path->input_advance_pending != 0u) {
		if (path->input_count == 0u)
			return false;
		path->previous = path->current;
		path->current = queue_pop(path);
		path->input_advance_pending--;
	}
	if (!path->current_valid) {
		if (path->input_count == 0u)
			return false;
		path->current = queue_pop(path);
		path->current_valid = true;
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
	const struct v34_receive_path *path)
{
	uint16_t phase = path->timing.phase;
	uint16_t remaining = (uint16_t)(path->timing.phase_limit - phase);

	return (struct v34_analytic_sample){
		.in_phase = interpolate(
			path->current.in_phase, phase,
			path->previous.in_phase, remaining),
		.quadrature = interpolate(
			path->current.quadrature, phase,
			path->previous.quadrature, remaining),
	};
}

static void advance_phase(struct v34_receive_path *path)
{
	uint32_t next = (uint32_t)path->timing.phase +
			path->timing.phase_increment;
	uint16_t advances = 0;

	while (next >= path->timing.phase_limit) {
		next -= path->timing.phase_limit;
		advances++;
	}
	path->timing.phase = (uint16_t)next;
	path->input_advance_pending = (uint8_t)advances;
}

static struct v34_analytic_sample rounded_equalizer_output(
	struct v34_equalizer_accumulator accumulator)
{
	return (struct v34_analytic_sample){
		.in_phase = wrap_s16(arithmetic_shift_right(
			(uint32_t)accumulator.in_phase + UINT32_C(0x2000), 14)),
		.quadrature = wrap_s16(arithmetic_shift_right(
			(uint32_t)accumulator.quadrature + UINT32_C(0x2000), 14)),
	};
}

static enum v34_receive_path_result drain_timing(
	struct v34_receive_path *path,
	struct v34_analytic_sample *symbol, bool *ready)
{
	while (advance_input(path)) {
		struct v34_analytic_sample interpolated =
			interpolate_current(path);
		int16_t timing_error;
		uint8_t output_index = path->timing_output_count;

		if (v34_timing_filter_observe(
			    &path->timing_filter, interpolated,
			    &timing_error) != V34_TIMING_FILTER_OK)
			return V34_RECEIVE_PATH_INVALID_STATE;
		path->timing_observations[output_index + 1u] = timing_error;
		if ((output_index & 1u) != 0u) {
			struct v34_analytic_sample filtered;

			if (v34_timing_filter_prefilter(
				    &path->timing_filter, &filtered) !=
					    V34_TIMING_FILTER_OK ||
			    v34_equalizer_push(&path->equalizer, filtered) !=
				    V34_EQUALIZER_OK)
				return V34_RECEIVE_PATH_INVALID_STATE;
		}
		path->timing_output_count++;
		advance_phase(path);
		if (path->timing_output_count ==
		    V34_RECEIVE_PATH_TIMING_OUTPUTS) {
			struct v34_equalizer_accumulator accumulator;
			uint16_t increment;

			if (v34_equalizer_filter(
				    &path->equalizer, &accumulator) !=
					    V34_EQUALIZER_OK ||
			    v34_timing_control_process(
				    &path->timing,
				    path->timing_observations,
				    &increment) != V34_TIMING_CONTROL_OK ||
			    increment != path->timing.phase_increment)
				return V34_RECEIVE_PATH_INVALID_STATE;
			path->timing_output_count = 0;
			*symbol = rounded_equalizer_output(accumulator);
			*ready = true;
			return V34_RECEIVE_PATH_SYMBOL_READY;
		}
	}
	return V34_RECEIVE_PATH_OK;
}

enum v34_receive_path_result v34_receive_path_sample(
	struct v34_receive_path *path, int16_t sample,
	struct v34_analytic_sample *symbol, bool *ready)
{
	struct v34_analytic_sample analytic;
	struct v34_analytic_sample gained;
	struct v34_analytic_sample demodulated;
	enum v34_receive_path_result result;

	if (!v34_receive_path_is_valid(path))
		return V34_RECEIVE_PATH_INVALID_STATE;
	if (symbol == NULL || ready == NULL ||
	    memory_regions_overlap(path, sizeof(*path), symbol, sizeof(*symbol)) ||
	    memory_regions_overlap(path, sizeof(*path), ready, sizeof(*ready)) ||
	    memory_regions_overlap(symbol, sizeof(*symbol), ready, sizeof(*ready)))
		return V34_RECEIVE_PATH_INVALID_ARGUMENT;
	if (path->input_count == V34_RECEIVE_PATH_INPUT_CAPACITY)
		return V34_RECEIVE_PATH_INVALID_STATE;
	if (v34_analytic_filter_process(
		    &path->analytic, sample, &analytic) !=
		    V34_ANALYTIC_FILTER_OK ||
	    v34_gain_control_process(&path->gain, analytic, &gained) !=
		    V34_GAIN_CONTROL_OK ||
	    v34_carrier_demodulator_process(
		    &path->carrier, gained, &demodulated) !=
		    V34_CARRIER_DEMODULATOR_OK)
		return V34_RECEIVE_PATH_INVALID_STATE;
	queue_push(path, demodulated);
	result = drain_timing(path, symbol, ready);
	if (result != V34_RECEIVE_PATH_OK &&
	    result != V34_RECEIVE_PATH_SYMBOL_READY)
		return result;
	if (!v34_receive_path_is_valid(path))
		return V34_RECEIVE_PATH_INVALID_STATE;
	if (result == V34_RECEIVE_PATH_OK)
		*ready = false;
	return result;
}
