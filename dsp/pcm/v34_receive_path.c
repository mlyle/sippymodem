// SPDX-License-Identifier: BSD-3-Clause

#include "v34_receive_path.h"

#include "memory_region.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

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

bool v34_receive_path_is_valid(const struct v34_receive_path *path)
{
	return path != NULL &&
	       v34_receive_timing_is_valid(&path->timing) &&
	       v34_equalizer_is_valid(&path->equalizer);
}

enum v34_receive_path_result v34_receive_path_init(
	struct v34_receive_path *path,
	const struct v34_phase3_profile *transmitting_profile)
{
	struct v34_receive_path initialized;

	if (path == NULL ||
	    !v34_phase3_profile_is_valid(transmitting_profile))
		return V34_RECEIVE_PATH_INVALID_ARGUMENT;
	if (v34_receive_timing_init(
		    &initialized.timing, transmitting_profile) !=
		    V34_RECEIVE_TIMING_OK ||
	    v34_equalizer_init(&initialized.equalizer) != V34_EQUALIZER_OK ||
	    !v34_receive_path_is_valid(&initialized))
		return V34_RECEIVE_PATH_INVALID_ARGUMENT;
	*path = initialized;
	return V34_RECEIVE_PATH_OK;
}

enum v34_receive_path_result v34_receive_path_sample(
	struct v34_receive_path *path, int16_t sample,
	struct v34_analytic_sample *symbol, bool *ready)
{
	struct v34_receive_path next;
	struct v34_receive_timing_samples samples;
	struct v34_equalizer_accumulator accumulator;
	struct v34_analytic_sample generated;
	enum v34_receive_timing_result result;

	if (!v34_receive_path_is_valid(path))
		return V34_RECEIVE_PATH_INVALID_STATE;
	if (symbol == NULL || ready == NULL ||
	    memory_regions_overlap(path, sizeof(*path), symbol, sizeof(*symbol)) ||
	    memory_regions_overlap(path, sizeof(*path), ready, sizeof(*ready)) ||
	    memory_regions_overlap(symbol, sizeof(*symbol), ready, sizeof(*ready)))
		return V34_RECEIVE_PATH_INVALID_ARGUMENT;
	next = *path;
	result = v34_receive_timing_sample(
		&next.timing, sample, &samples);
	if (result != V34_RECEIVE_TIMING_OK &&
	    result != V34_RECEIVE_TIMING_SAMPLES_READY)
		return V34_RECEIVE_PATH_INVALID_STATE;
	if (result == V34_RECEIVE_TIMING_OK) {
		if (!v34_receive_path_is_valid(&next))
			return V34_RECEIVE_PATH_INVALID_STATE;
		*path = next;
		*ready = false;
		return V34_RECEIVE_PATH_OK;
	}
	for (size_t index = 0; index < samples.count; ++index) {
		if (v34_equalizer_push(
			    &next.equalizer, samples.sample[index]) !=
		    V34_EQUALIZER_OK)
			return V34_RECEIVE_PATH_INVALID_STATE;
	}
	if (!samples.symbol_complete) {
		if (!v34_receive_path_is_valid(&next))
			return V34_RECEIVE_PATH_INVALID_STATE;
		*path = next;
		*ready = false;
		return V34_RECEIVE_PATH_OK;
	}
	if (v34_equalizer_filter(&next.equalizer, &accumulator) !=
	    V34_EQUALIZER_OK)
		return V34_RECEIVE_PATH_INVALID_STATE;
	generated = rounded_equalizer_output(accumulator);
	if (!v34_receive_path_is_valid(&next))
		return V34_RECEIVE_PATH_INVALID_STATE;
	*path = next;
	*symbol = generated;
	*ready = true;
	return V34_RECEIVE_PATH_SYMBOL_READY;
}
