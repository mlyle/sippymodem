// SPDX-License-Identifier: BSD-3-Clause

#include "v34_timing_control.h"

#include "memory_region.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool role_is_valid(enum v34_station_role role)
{
	return role == V34_CALLING_STATION ||
	       role == V34_ANSWERING_STATION;
}

static int32_t wrap_s32(uint32_t value)
{
	return value <= INT32_MAX ?
		       (int32_t)value :
		       (int32_t)((int64_t)value - (INT64_C(1) << 32));
}

static int16_t wrap_s16(int32_t value)
{
	uint16_t word = (uint16_t)(uint32_t)value;

	return word <= INT16_MAX ?
		       (int16_t)word : (int16_t)((int32_t)word - 65536);
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
	return wrap_s32(shifted);
}

static uint32_t multiply_low(int32_t left, int32_t right)
{
	return (uint32_t)((uint64_t)(uint32_t)left *
			  (uint64_t)(uint32_t)right);
}

static int32_t divide_s32(uint32_t dividend, int32_t divisor)
{
	return divisor == 0 ?
		       0 :
		       (int32_t)((int64_t)wrap_s32(dividend) / divisor);
}

static uint32_t gcd_u32(uint32_t left, uint32_t right)
{
	while (right != 0) {
		uint32_t remainder = left % right;

		left = right;
		right = remainder;
	}
	return left;
}

static bool mode_is_valid(enum v34_timing_control_mode mode)
{
	return mode >= V34_TIMING_CONTROL_LOCKED &&
	       mode <= V34_TIMING_CONTROL_HOLD;
}

static bool control_header_is_valid(
	const struct v34_timing_control *control)
{
	return control != NULL && control->initialized &&
	       role_is_valid(control->local_role) &&
	       mode_is_valid(control->mode) && control->phase_limit > 0 &&
	       control->phase_limit <= INT16_MAX &&
	       control->phase < control->phase_limit &&
	       control->nominal_increment > 0 &&
	       control->nominal_increment < control->phase_limit &&
	       control->phase_increment > 0 &&
	       (uint32_t)control->phase_increment <
		       (uint32_t)control->phase_limit * 2u &&
	       control->phase_residue >= -0x4000 &&
	       control->phase_residue < 0x4000 &&
	       control->average_limit > 0 &&
	       control->early_index >= 1 && control->early_index <= 2 &&
	       control->late_index >= 1 && control->late_index <= 2 &&
	       control->early_index != control->late_index &&
	       control->nominal_symbol_rate > 0 &&
	       (!control->full_pull_in ||
		control->mode == V34_TIMING_CONTROL_INITIAL_PHASE);
}

bool v34_timing_control_is_valid(
	const struct v34_timing_control *control)
{
	return control_header_is_valid(control) &&
	       control->accumulated_count < control->average_limit;
}

enum v34_timing_control_result v34_timing_control_init(
	struct v34_timing_control *control, uint32_t sample_rate_hz,
	uint32_t symbol_rate_numerator, uint32_t symbol_rate_denominator,
	enum v34_station_role local_role)
{
	struct v34_timing_control initialized;
	uint64_t wide_numerator;
	uint64_t wide_denominator;
	uint64_t nominal_rate;
	uint32_t timing_numerator;
	uint32_t timing_denominator;
	uint32_t divisor;
	uint32_t scale;
	uint64_t increment;
	uint64_t limit;

	if (control == NULL || sample_rate_hz == 0 ||
	    symbol_rate_numerator == 0 || symbol_rate_denominator == 0 ||
	    !role_is_valid(local_role))
		return V34_TIMING_CONTROL_INVALID_ARGUMENT;
	wide_numerator = (uint64_t)sample_rate_hz * symbol_rate_denominator;
	wide_denominator = (uint64_t)symbol_rate_numerator * 4u;
	nominal_rate = ((uint64_t)symbol_rate_numerator +
			(symbol_rate_denominator / 2u)) /
		       symbol_rate_denominator;
	if (wide_numerator > UINT32_MAX || wide_denominator > UINT32_MAX ||
	    nominal_rate == 0 || nominal_rate > INT16_MAX)
		return V34_TIMING_CONTROL_INVALID_ARGUMENT;
	timing_numerator = (uint32_t)wide_numerator;
	timing_denominator = (uint32_t)wide_denominator;
	divisor = gcd_u32(timing_numerator, timing_denominator);
	if (divisor == 0)
		return V34_TIMING_CONTROL_INVALID_ARGUMENT;
	timing_numerator /= divisor;
	timing_denominator /= divisor;
	scale = (16000u + timing_denominator / 2u) / timing_denominator;
	increment = (uint64_t)timing_numerator * scale;
	limit = (uint64_t)timing_denominator * scale;
	if (scale == 0 || increment == 0 || increment > UINT16_MAX ||
	    limit == 0 || limit > INT16_MAX || increment >= limit ||
	    (nominal_rate >> 3) == 0)
		return V34_TIMING_CONTROL_INVALID_ARGUMENT;

	memset(&initialized, 0, sizeof(initialized));
	initialized.phase = (uint16_t)(limit / 2u);
	initialized.phase_increment = (uint16_t)increment;
	initialized.phase_limit = (uint16_t)limit;
	initialized.nominal_increment = (uint16_t)increment;
	initialized.average_limit = (uint16_t)(nominal_rate >> 3);
	initialized.timeout = -1;
	initialized.proportional_gain = 0x1770;
	initialized.integral_gain = 0x005a;
	initialized.early_index = 1;
	initialized.late_index = 2;
	initialized.nominal_symbol_rate = (uint16_t)nominal_rate;
	initialized.local_role = local_role;
	initialized.mode = V34_TIMING_CONTROL_LOCKED;
	initialized.initialized = true;
	if (!v34_timing_control_is_valid(&initialized))
		return V34_TIMING_CONTROL_INVALID_ARGUMENT;
	*control = initialized;
	return V34_TIMING_CONTROL_OK;
}

enum v34_timing_control_result v34_timing_control_begin_acquisition(
	struct v34_timing_control *control, bool full_pull_in)
{
	struct v34_timing_control next;

	if (!v34_timing_control_is_valid(control))
		return V34_TIMING_CONTROL_INVALID_STATE;
	next = *control;
	next.mode = V34_TIMING_CONTROL_INITIAL_PHASE;
	next.full_pull_in = full_pull_in;
	*control = next;
	return V34_TIMING_CONTROL_OK;
}

static int16_t negate_s16(int16_t value)
{
	return (int16_t)(uint16_t)(0u - (uint16_t)value);
}

static int16_t timing_curve_value(int16_t value)
{
	int64_t wide = value;
	int64_t result = wide * wide * -0x15 - 0x162 + wide * 0x345;

	return (int16_t)(uint16_t)result;
}

static void set_initial_phase(
	struct v34_timing_control *control,
	const int16_t observations[V34_TIMING_CONTROL_OBSERVATIONS])
{
	int16_t index = 1;
	int16_t first;
	int16_t second;
	int32_t denominator;
	int32_t target = 0;
	int16_t best_error = 32000;
	int16_t best_index = 0;
	uint16_t phase;

	if ((int32_t)observations[1] * observations[2] < 0) {
		do {
			index = (int16_t)(uint16_t)(index + 1);
		} while (index < 5 &&
			 (int32_t)observations[index] *
				 observations[index + 1] < 0);
		if (index == 5)
			index = 1;
	}
	first = observations[index];
	second = observations[index + 1];
	if (first < 0) {
		first = negate_s16(first);
		second = negate_s16(second);
	}
	if (index == 2) {
		control->early_index = 2;
		control->late_index = 1;
	} else {
		control->early_index = 1;
		control->late_index = 2;
	}
	denominator = (int32_t)first + second;
	if (denominator != 0) {
		uint32_t numerator = (uint32_t)(
			(denominator >> 1) +
			((int32_t)second - first) * 0x2000);

		target = divide_s32(numerator, denominator);
	}
	for (int16_t candidate = 0; candidate < 20; ++candidate) {
		int16_t high = timing_curve_value(
			(int16_t)(candidate + 0x14));
		int16_t low = timing_curve_value(candidate);
		int32_t sum = (int32_t)high + low;
		int32_t ratio = 0;
		int32_t difference;
		int16_t error;

		if (sum != 0) {
			uint32_t numerator = (uint32_t)(
				(sum >> 1) +
				((int32_t)high - low) * 0x2000);

			ratio = divide_s32(numerator, sum);
		}
		difference = target - ratio;
		error = wrap_s16(arithmetic_shift_right(
			multiply_low(difference, difference) +
				UINT32_C(0x1000),
			13));
		if (error < best_error) {
			best_error = error;
			best_index = candidate;
		}
	}
	phase = (uint16_t)(control->phase +
			   (uint16_t)(int16_t)((10 - best_index) * 0x230));
	control->phase = phase < control->phase_limit ?
				 phase : (uint16_t)(control->phase_limit - 1u);
}

static void set_mode_parameters(struct v34_timing_control *control)
{
	bool caller = control->local_role == V34_CALLING_STATION;

	switch (control->mode) {
	case V34_TIMING_CONTROL_FAST_PULL_IN:
		control->proportional_gain = 0x36b0;
		control->integral_gain = 0;
		control->timeout = 400;
		control->average_limit =
			(uint16_t)(control->nominal_symbol_rate >> 3);
		break;
	case V34_TIMING_CONTROL_PULL_IN:
		control->proportional_gain = 0x2ee0;
		control->integral_gain = 0x00d2;
		control->timeout = 1000;
		break;
	case V34_TIMING_CONTROL_WIDE_TRACK:
		control->proportional_gain = 0x1770;
		control->integral_gain = 0x005a;
		control->timeout = 1000;
		break;
	case V34_TIMING_CONTROL_MEDIUM_TRACK:
		control->proportional_gain = 0x0dac;
		control->integral_gain = 0x001e;
		control->timeout = caller ? -1 : 1000;
		break;
	case V34_TIMING_CONTROL_NARROW_TRACK:
		control->proportional_gain = caller ? 0x0dac : 0x07d0;
		control->integral_gain = caller ? 0x0003 : 0x000a;
		control->timeout = 2000;
		break;
	case V34_TIMING_CONTROL_FINE_TRACK:
		control->proportional_gain = caller ? 0x03e8 : 0x05dc;
		control->integral_gain = 0x0002;
		control->timeout = caller ? 2000 : 4000;
		break;
	case V34_TIMING_CONTROL_HOLD:
		control->proportional_gain = 0x01f4;
		control->integral_gain = 0x0001;
		control->timeout = -1;
		break;
	case V34_TIMING_CONTROL_LOCKED:
	case V34_TIMING_CONTROL_INITIAL_PHASE:
		break;
	}
}

enum v34_timing_control_result v34_timing_control_enter_hold(
	struct v34_timing_control *control)
{
	struct v34_timing_control next;

	if (!v34_timing_control_is_valid(control) ||
	    control->mode == V34_TIMING_CONTROL_LOCKED ||
	    control->mode == V34_TIMING_CONTROL_INITIAL_PHASE)
		return V34_TIMING_CONTROL_INVALID_STATE;
	next = *control;
	next.mode = V34_TIMING_CONTROL_HOLD;
	next.state_count = 0;
	set_mode_parameters(&next);
	if (!v34_timing_control_is_valid(&next))
		return V34_TIMING_CONTROL_INVALID_STATE;
	*control = next;
	return V34_TIMING_CONTROL_OK;
}

static int32_t timing_phase_error(
	const struct v34_timing_control *control,
	const int16_t observations[V34_TIMING_CONTROL_OBSERVATIONS])
{
	size_t early = control->early_index;
	size_t late = control->late_index;
	int16_t early_real = observations[early];
	int16_t early_imag = observations[early + 2u];
	int16_t late_real = observations[late];
	int16_t late_imag = observations[late + 2u];
	uint32_t early_energy = multiply_low(early_real, early_real) +
				(uint32_t)multiply_low(early_imag, early_imag);
	uint32_t late_energy = multiply_low(late_real, late_real) +
			       (uint32_t)multiply_low(late_imag, late_imag);
	int16_t shift_count = 0;
	uint32_t mask = UINT32_C(0x80000000);
	unsigned int shift;
	uint32_t early_normalized;
	uint32_t late_normalized;
	uint32_t sum;
	uint32_t numerator;

	if (wrap_s32(early_energy) >= 0 && wrap_s32(late_energy) >= 0) {
		do {
			mask >>= 1;
			shift_count = (int16_t)(uint16_t)(shift_count + 1);
			if ((early_energy & mask) != 0)
				break;
		} while ((late_energy & mask) == 0 && shift_count <= 15);
	}
	shift = (unsigned int)(16 - shift_count) & 31u;
	early_normalized = (uint16_t)arithmetic_shift_right(
		early_energy, shift);
	late_normalized = (uint16_t)arithmetic_shift_right(
		late_energy, shift);
	if ((early_normalized | late_normalized) == 0)
		return 0;
	sum = early_normalized + late_normalized;
	numerator = ((late_normalized - early_normalized) << 15) +
		    (uint32_t)arithmetic_shift_right(sum, 1);
	return divide_s32(numerator, (int32_t)sum);
}

static void update_offset(struct v34_timing_control *control,
			  int16_t phase_delta)
{
	int16_t accumulated = wrap_s16(
		(int32_t)control->accumulated_delta + phase_delta);
	uint16_t count = (uint16_t)(control->accumulated_count + 1u);

	if (count < control->average_limit) {
		control->accumulated_count = count;
		control->accumulated_delta = accumulated;
		return;
	}
	control->offset_ppm = wrap_s16(divide_s32(
		(uint32_t)((int32_t)(int16_t)control->nominal_increment / 2 +
			   divide_s32(
				   (uint32_t)((int32_t)accumulated * 10000 +
					      (int32_t)(count >> 1)),
				   count) *
					   100),
		(int16_t)control->nominal_increment));
	control->accumulated_count = 0;
	control->accumulated_delta = 0;
}

enum v34_timing_control_result v34_timing_control_process(
	struct v34_timing_control *control,
	const int16_t observations[V34_TIMING_CONTROL_OBSERVATIONS],
	uint16_t *phase_increment)
{
	struct v34_timing_control next;
	int32_t timing_step;
	uint32_t phase_accumulator;
	int16_t phase_delta;

	if (!v34_timing_control_is_valid(control))
		return V34_TIMING_CONTROL_INVALID_STATE;
	if (observations == NULL || phase_increment == NULL ||
	    memory_regions_overlap(
		    control, sizeof(*control), observations,
		    sizeof(*observations) * V34_TIMING_CONTROL_OBSERVATIONS) ||
	    memory_regions_overlap(
		    control, sizeof(*control), phase_increment,
		    sizeof(*phase_increment)) ||
	    memory_regions_overlap(
		    observations,
		    sizeof(*observations) * V34_TIMING_CONTROL_OBSERVATIONS,
		    phase_increment, sizeof(*phase_increment)))
		return V34_TIMING_CONTROL_INVALID_ARGUMENT;
	next = *control;
	if (next.mode == V34_TIMING_CONTROL_INITIAL_PHASE) {
		next.state_count = 0;
		set_initial_phase(&next, observations);
		next.mode = next.full_pull_in ?
				    V34_TIMING_CONTROL_FAST_PULL_IN :
				    V34_TIMING_CONTROL_NARROW_TRACK;
		next.full_pull_in = false;
		set_mode_parameters(&next);
	}
	if (next.timeout != -1 && next.mode != V34_TIMING_CONTROL_LOCKED) {
		uint16_t count = (uint16_t)(next.state_count + 1u);

		if (count == (uint16_t)next.timeout) {
			next.state_count = 0;
			next.mode = (enum v34_timing_control_mode)(next.mode + 1);
			set_mode_parameters(&next);
		} else {
			next.state_count = count;
		}
	}
	if (next.mode == V34_TIMING_CONTROL_LOCKED) {
		timing_step = next.integrator;
	} else {
		int32_t error = timing_phase_error(&next, observations);
		int32_t integral_delta = arithmetic_shift_right(
			multiply_low(next.integral_gain, error) +
				UINT32_C(0x4000),
			15);
		int32_t proportional = arithmetic_shift_right(
			multiply_low(next.proportional_gain, error) +
				UINT32_C(0x200),
			11);

		next.integrator = wrap_s32(
			(uint32_t)next.integrator + (uint32_t)integral_delta);
		timing_step = wrap_s32(
			(uint32_t)next.integrator + (uint32_t)proportional);
	}
	phase_accumulator = (uint32_t)timing_step +
			    (uint32_t)next.phase_residue;
	phase_delta = wrap_s16(arithmetic_shift_right(
		phase_accumulator + UINT32_C(0x4000), 15));
	next.phase_residue = wrap_s32(
		phase_accumulator -
		((uint32_t)(int32_t)phase_delta << 15));
	next.phase_increment = (uint16_t)(
		next.nominal_increment + (uint16_t)phase_delta);
	update_offset(&next, phase_delta);
	if (!v34_timing_control_is_valid(&next))
		return V34_TIMING_CONTROL_INVALID_STATE;
	*control = next;
	*phase_increment = next.phase_increment;
	return V34_TIMING_CONTROL_OK;
}
