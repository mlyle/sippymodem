// SPDX-License-Identifier: BSD-3-Clause

#include "symbol_timing.h"
#include "agc.h"
#include "fixed_phase.h"
#include "integer_math.h"
#include "memory_region.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

enum {
	TIMING_PHASES = 10,
	SAMPLES_PER_SYMBOL = 3,
	COARSE_GROUP_SYMBOLS = 3,
	FINE_GROUP_SYMBOLS = 16,
	ACQUISITION_DELAY_GROUPS = 70,
	STARTUP_LEVEL_THRESHOLD = 0x05dc,
	SIGNAL_ON_THRESHOLD = 2,
	SIGNAL_OFF_THRESHOLD = 1,
	FINE_ENTER_ERROR = 0x00c8 / 8,
	FINE_EXIT_ERROR = 0x2666 / 8,
};

_Static_assert(COARSE_GROUP_SYMBOLS * SAMPLES_PER_SYMBOL == 9,
	"timing corrections must be separated by nine outputs");
_Static_assert(V32_SYMBOL_TIMING_MAX_INPUT_SAMPLES + 2 + 4 ==
		       V32_SYMBOL_TIMING_MAX_OUTPUT_SAMPLES,
	"timing output storage must cover every valid cursor");

/* Ten phases of an 18-tap interpolator plus the adjacent endpoint. */
static const int16_t INTERPOLATOR_SOURCE[181] = {
	45, 62, 75, 82, 81, 72, 52, 24, -10, -50,
	-89, -126, -153, -169, -169, -152, -116, -64, 0, 72,
	146, 213, 266, 298, 303, 278, 223, 139, 33, -86,
	-210, -325, -419, -480, -499, -470, -392, -267, -105, 82,
	279, 467, 625, 735, 783, 758, 656, 480, 241, -42,
	-347, -645, -907, -1102, -1207, -1202, -1079, -838, -492, -65,
	408, 888, 1327, 1679, 1902, 1959, 1829, 1503, 992, 324,
	-456, -1288, -2099, -2808, -3335, -3604, -3551, -3128, -2310, -1096,
	487, 2388, 4528, 6812, 9126, 11352, 13371, 15074, 16364, 17168,
	17442, 17168, 16364, 15074, 13371, 11352, 9126, 6812, 4528, 2388,
	487, -1096, -2310, -3128, -3551, -3604, -3335, -2808, -2099, -1288,
	-456, 324, 992, 1503, 1829, 1959, 1902, 1679, 1327, 888,
	408, -65, -492, -838, -1079, -1202, -1207, -1102, -907, -645,
	-347, -42, 241, 480, 656, 758, 783, 735, 625, 467,
	279, 82, -105, -267, -392, -470, -499, -480, -419, -325,
	-210, -86, 33, 139, 223, 278, 303, 298, 266, 213,
	146, 72, 0, -64, -116, -152, -169, -169, -153, -126,
	-89, -50, -10, 24, 52, 72, 81, 82, 75, 62, 45,
};

static const int16_t ERROR_FILTER[11] = {
	-28156, 16128, 28156, 16128, 14078, 8128,
	-14078, 8128, 992, -15360, 14399,
};

static const int16_t DETECTOR_I[3] = { 16384, -8192, -8192 };
static const int16_t DETECTOR_Q[3] = { 0, 14189, -14189 };
static const int16_t PROPORTIONAL_GAIN[3] = { 2336, 3049, 1524 };
static const int16_t INTEGRAL_GAIN[3] = { 0, 17, 8 };

static int16_t filter_symbol(const struct v32_symbol_timing *timing)
{
	uint32_t accumulator = 0;
	size_t history = timing->history_index;

	for (size_t tap = 0; tap < V32_SYMBOL_TIMING_HISTORY; ++tap) {
		int32_t product = (int32_t)timing->sample_history[history] *
			timing->interpolator[timing->polyphase_index +
					     tap * TIMING_PHASES];

		accumulator += (uint32_t)product;
		history = history == 0 ? V32_SYMBOL_TIMING_HISTORY - 1u :
					 history - 1u;
	}
	return wrap_s16_from_s32(
		wrap_s16_from_s32(
			arithmetic_shift_right_u32(accumulator, 15)) * 2);
}

static int16_t filter_error(
	struct v32_symbol_timing *timing, int16_t symbol)
{
	int16_t *state = timing->error_state;
	int32_t scaled = (int32_t)symbol * 0x200;
	int16_t s0 = state[0], s1 = state[1], s2 = state[2];
	int16_t s3 = state[3], s4 = state[4], s5 = state[5];
	int32_t y0_acc = wrap_s32_from_u32((uint32_t)scaled -
		(uint32_t)((int32_t)ERROR_FILTER[0] * s0) -
		(uint32_t)((int32_t)ERROR_FILTER[1] * s1));
	int32_t y1_acc = wrap_s32_from_u32((uint32_t)scaled -
		(uint32_t)((int32_t)ERROR_FILTER[2] * s2) -
		(uint32_t)((int32_t)ERROR_FILTER[3] * s3));
	int16_t y0 = wrap_s16_from_s32(
		arithmetic_shift_right_u32((uint32_t)y0_acc, 14));
	int16_t y1 = wrap_s16_from_s32(
		arithmetic_shift_right_u32((uint32_t)y1_acc, 14));
	int16_t first = wrap_s16_from_s32(
		y1 - multiply_shift_s16(ERROR_FILTER[6], s2, 14));
	int16_t second = wrap_s16_from_s32(
		y0 - multiply_shift_s16(ERROR_FILTER[4], s0, 14));
	int32_t mixed_acc = wrap_s32_from_u32(
		(uint32_t)((int32_t)first * second) +
		(uint32_t)((int32_t)multiply_shift_s16(
			ERROR_FILTER[7], s2, 14) *
			multiply_shift_s16(ERROR_FILTER[5], s0, 14)));
	int16_t mixed = wrap_s16_from_s32(
		arithmetic_shift_right_u32((uint32_t)mixed_acc, 11));
	int32_t y2_acc = wrap_s32_from_u32(
		(uint32_t)((int32_t)ERROR_FILTER[8] * mixed) +
		(uint32_t)((int32_t)ERROR_FILTER[9] * s4) -
		(uint32_t)((int32_t)ERROR_FILTER[10] * s5));
	int16_t y2 = wrap_s16_from_s32(
		arithmetic_shift_right_u32((uint32_t)y2_acc, 14));

	state[1] = s0;
	state[0] = y0;
	state[3] = s2;
	state[2] = y1;
	state[5] = s4;
	state[4] = y2;
	return wrap_s16_from_s32(y2 - s5);
}

static void interpolate_coefficients(
	struct v32_symbol_timing *timing)
{
	int16_t fraction = timing->fractional_phase_q11;
	int16_t left = wrap_s16_from_s32(0x7fff - fraction * 0x10);
	int16_t right = wrap_s16_from_s32(fraction * 0x10);

	for (size_t index = 0; index < V32_SYMBOL_TIMING_COEFFICIENTS;
	     ++index) {
		int16_t value = multiply_shift_s16(
			INTERPOLATOR_SOURCE[index], left, 15);

		timing->interpolator[index] = wrap_s16_from_s32(
			value + multiply_shift_s16(
				INTERPOLATOR_SOURCE[index + 1u], right, 15));
	}
}

static void adjust_phase(
	struct v32_symbol_timing *timing, int16_t angle_error)
{
	int16_t mode = (int16_t)timing->mode;
	int16_t proportional = multiply_shift_s16(
		PROPORTIONAL_GAIN[mode], angle_error, 12);
	int16_t integral_step = wrap_s16_from_s32(
		arithmetic_shift_right_u32(
		(uint32_t)((int32_t)INTEGRAL_GAIN[mode] * angle_error +
			   0x800), 12));
	int16_t loop_output = wrap_s16_from_s32(
		proportional + timing->loop_integral);
	int32_t adjust = arithmetic_shift_right_u32(
		(uint32_t)((int32_t)loop_output * -10), 4);
	int16_t whole;
	int16_t phase;
	int16_t fraction;

	timing->loop_integral = wrap_s16_from_s32(
		timing->loop_integral + integral_step);
	if (adjust > 0x5000)
		adjust = 0x5000;
	else if (adjust < -0x4800)
		adjust = -0x4800;
	whole = wrap_s16_from_s32(
		arithmetic_shift_right_u32((uint32_t)adjust, 11));
	if (adjust < 0)
		whole = wrap_s16_from_s32(whole + 1);
	phase = wrap_s16_from_s32(timing->polyphase_index + whole);
	fraction = wrap_s16_from_s32(
		wrap_s16_from_s32(adjust) - whole * 0x800 +
		timing->fractional_phase_q11);
	if (fraction < 0) {
		phase = wrap_s16_from_s32(phase - 1);
		fraction = wrap_s16_from_s32(fraction + 0x800);
	}
	timing->fractional_phase_q11 = fraction;
	timing->polyphase_index = phase;
	if (fraction > 0x7ff) {
		timing->fractional_phase_q11 =
			wrap_s16_from_s32(fraction - 0x800);
		timing->polyphase_index = wrap_s16_from_s32(phase + 1);
	}
}

static void update_loop(
	struct v32_symbol_timing *timing, int32_t detector_i,
	int32_t detector_q)
{
	int16_t i = wrap_s16_from_s32(
		arithmetic_shift_right_u32((uint32_t)detector_i, 16));
	int16_t q = wrap_s16_from_s32(
		arithmetic_shift_right_u32((uint32_t)detector_q, 16));
	uint16_t angle;
	int16_t angle_error;
	int16_t absolute_error;

	if (!timing->signal_detected)
		return;
	timing->detector_i_accumulator = detector_i;
	timing->detector_q_accumulator = detector_q;
	if (timing->mode == V32_SYMBOL_TIMING_FINE &&
	    (uint16_t)(i + 0x31) <= 0x62)
		return;
	angle = fixed_phase_atan2_u15(q, i);
	if (angle > 0x4000)
		angle = (uint16_t)(angle - 0x8000);
	angle_error = wrap_s16_from_s32(arithmetic_shift_right_u32(
		(uint32_t)(SAMPLES_PER_SYMBOL * wrap_s16_from_s32(angle)), 3));
	timing->smoothed_error = wrap_s16_from_s32(
		arithmetic_shift_right_u32(
			(uint32_t)((int32_t)timing->smoothed_error * 15), 4) +
		arithmetic_shift_right_u32(
			(uint32_t)(int32_t)angle_error, 4));
	if (!timing->tracking_enabled)
		return;
	if (timing->acquisition_groups > ACQUISITION_DELAY_GROUPS) {
		absolute_error = absolute_wrap_s16(timing->smoothed_error);
		if (absolute_error >= FINE_EXIT_ERROR &&
		    timing->mode == V32_SYMBOL_TIMING_FINE) {
			timing->mode = V32_SYMBOL_TIMING_COARSE;
			timing->loop_integral = multiply_shift_s16(
				0x2000, timing->loop_integral, 15);
		}
		if (absolute_error <= FINE_ENTER_ERROR &&
		    timing->mode == V32_SYMBOL_TIMING_COARSE) {
			timing->mode = V32_SYMBOL_TIMING_FINE;
			timing->loop_integral = multiply_shift_s16(
				0x4000, timing->loop_integral, 12);
		}
	} else {
		timing->loop_integral = 0;
		if (timing->acquisition_groups < UINT8_MAX)
			timing->acquisition_groups++;
		timing->mode = timing->acquisition_groups <=
				       ACQUISITION_DELAY_GROUPS ?
			V32_SYMBOL_TIMING_ACQUISITION :
			V32_SYMBOL_TIMING_COARSE;
	}
	adjust_phase(timing, angle_error);
	if (timing->mode == V32_SYMBOL_TIMING_FINE)
		interpolate_coefficients(timing);
}

static void feed_sample(
	struct v32_symbol_timing *timing, int16_t sample)
{
	timing->history_index = (uint8_t)(
		(timing->history_index + 1u) % V32_SYMBOL_TIMING_HISTORY);
	timing->sample_history[timing->history_index] = sample;
	if (timing->startup_gate_enabled) {
		timing->startup_samples[timing->startup_index] = sample;
		timing->startup_index = (uint8_t)(
			(timing->startup_index + 1u) %
			V32_SYMBOL_TIMING_STARTUP_SAMPLES);
	}
}

static uint8_t group_limit(const struct v32_symbol_timing *timing)
{
	return timing->mode == V32_SYMBOL_TIMING_FINE ?
		FINE_GROUP_SYMBOLS : COARSE_GROUP_SYMBOLS;
}

bool v32_symbol_timing_is_valid(
	const struct v32_symbol_timing *timing)
{
	return timing != NULL &&
	       timing->mode >= V32_SYMBOL_TIMING_ACQUISITION &&
	       timing->mode <= V32_SYMBOL_TIMING_FINE &&
	       timing->history_index < V32_SYMBOL_TIMING_HISTORY &&
	       timing->acquisition_groups <= ACQUISITION_DELAY_GROUPS + 1u &&
	       timing->detector_groups < group_limit(timing) &&
	       timing->detector_sample < SAMPLES_PER_SYMBOL &&
	       timing->samples_until_output <= 2u &&
	       timing->startup_index < V32_SYMBOL_TIMING_STARTUP_SAMPLES &&
	       timing->fractional_phase_q11 >= 0 &&
	       timing->fractional_phase_q11 < 0x800 &&
	       timing->polyphase_index >= 0 &&
	       timing->polyphase_index < TIMING_PHASES;
}

enum v32_symbol_timing_result v32_symbol_timing_init(
	struct v32_symbol_timing *timing)
{
	struct v32_symbol_timing initialized;

	if (timing == NULL)
		return V32_SYMBOL_TIMING_INVALID_ARGUMENT;
	memset(&initialized, 0, sizeof(initialized));
	memcpy(initialized.interpolator, INTERPOLATOR_SOURCE,
	       sizeof(initialized.interpolator));
	initialized.mode = V32_SYMBOL_TIMING_ACQUISITION;
	initialized.tracking_enabled = true;
	initialized.startup_gate_enabled = true;
	initialized.samples_until_output = 1;
	if (!v32_symbol_timing_is_valid(&initialized))
		return V32_SYMBOL_TIMING_INVALID_STATE;
	*timing = initialized;
	return V32_SYMBOL_TIMING_OK;
}

enum v32_symbol_timing_result v32_symbol_timing_set_tracking(
	struct v32_symbol_timing *timing, bool enabled)
{
	if (!v32_symbol_timing_is_valid(timing))
		return V32_SYMBOL_TIMING_INVALID_STATE;
	timing->tracking_enabled = enabled;
	return V32_SYMBOL_TIMING_OK;
}

static bool arguments_are_valid(
	const struct v32_symbol_timing *timing,
	const int16_t *input, size_t count, size_t *output_count)
{
	size_t input_bytes = count * sizeof(*input);

	return count <= V32_SYMBOL_TIMING_MAX_INPUT_SAMPLES &&
	       (input != NULL || count == 0) && output_count != NULL &&
	       !memory_regions_overlap(input, input_bytes,
				       timing, sizeof(*timing)) &&
	       !memory_regions_overlap(output_count, sizeof(*output_count),
				       timing, sizeof(*timing)) &&
	       !memory_regions_overlap(input, input_bytes,
				       output_count, sizeof(*output_count));
}

enum v32_symbol_timing_result v32_symbol_timing_process(
	struct v32_symbol_timing *timing,
	const int16_t *input, size_t count,
	int16_t *output, size_t output_capacity,
	size_t *output_count)
{
	struct v32_symbol_timing next;
	int16_t recovered[V32_SYMBOL_TIMING_MAX_OUTPUT_SAMPLES];
	const int16_t *input_start = input;
	size_t remaining = count;
	size_t produced = 0;

	if (!v32_symbol_timing_is_valid(timing))
		return V32_SYMBOL_TIMING_INVALID_STATE;
	if (!arguments_are_valid(timing, input, count, output_count))
		return V32_SYMBOL_TIMING_INVALID_ARGUMENT;
	next = *timing;
	while (remaining != 0) {
		uint8_t needed = next.samples_until_output;

		if (remaining < needed) {
			size_t partial = remaining;

			while (partial-- != 0)
				feed_sample(&next, *input++);
			next.samples_until_output = (uint8_t)(needed -
							       remaining);
			remaining = 0;
			break;
		}
		for (uint8_t index = 0; index < needed; ++index)
			feed_sample(&next, *input++);
		remaining -= needed;
		if (produced == V32_SYMBOL_TIMING_MAX_OUTPUT_SAMPLES)
			return V32_SYMBOL_TIMING_INVALID_STATE;
		int16_t symbol = filter_symbol(&next);
		int16_t error = filter_error(&next, symbol);

		if (next.startup_gate_enabled) {
			int16_t level;

			if (agc_measure_level_s16(
				    next.startup_samples,
				    V32_SYMBOL_TIMING_STARTUP_SAMPLES,
				    &level) != AGC_OK)
				return V32_SYMBOL_TIMING_INVALID_STATE;
			if (level > STARTUP_LEVEL_THRESHOLD)
				next.startup_gate_enabled = false;
			else
				error = 0;
		}
		uint8_t detector_sample = next.detector_sample;
		int32_t next_i = wrap_s32_from_u32(
			(uint32_t)next.detector_i_accumulator +
			(uint32_t)arithmetic_shift_right_u32(
				(uint32_t)((int32_t)error *
					   DETECTOR_I[detector_sample]), 3));
		int32_t next_q = wrap_s32_from_u32(
			(uint32_t)next.detector_q_accumulator -
			(uint32_t)arithmetic_shift_right_u32(
				(uint32_t)((int32_t)error *
					   DETECTOR_Q[detector_sample]), 3));
		int32_t level = wrap_s16_from_s32((int32_t)(
			((uint32_t)(int32_t)next.detector_level * 15u) >> 4));

		level += arithmetic_shift_right_u32(
			(uint32_t)(int32_t)absolute_wrap_s16(error), 1);
		if (level > INT16_MAX)
			level = INT16_MAX;
		next.detector_level = wrap_s16_from_s32(level);
		next.detector_sample++;
		if (next.detector_sample < SAMPLES_PER_SYMBOL) {
			next.detector_i_accumulator = next_i;
			next.detector_q_accumulator = next_q;
		} else {
			next.detector_sample = 0;
			next.detector_groups++;
			if (next.detector_groups < group_limit(&next)) {
				next.detector_i_accumulator = next_i;
				next.detector_q_accumulator = next_q;
			} else {
				if (next.detector_level >= SIGNAL_ON_THRESHOLD)
					next.signal_detected = true;
				if (next.detector_level <= SIGNAL_OFF_THRESHOLD)
					next.signal_detected = false;
				update_loop(&next, next_i, next_q);
				next.detector_groups = 0;
				next.detector_i_accumulator = 0;
				next.detector_q_accumulator = 0;
			}
		}
		/*
		 * A phase correction occurs only after a three-symbol detector
		 * group, so at least nine outputs separate corrections.  A valid
		 * cursor can begin with two zero-input outputs (one pending output
		 * and one immediately induced correction).  The remaining forty
		 * output opportunities contain at most four corrections, bounding
		 * a 36-sample call to 36 + 2 + 4 == 42 outputs.
		 */
		int16_t phase = wrap_s16_from_s32(
			next.polyphase_index + TIMING_PHASES);
		uint8_t advance = 0;

		while (phase >= TIMING_PHASES) {
			phase = wrap_s16_from_s32(phase - TIMING_PHASES);
			advance++;
		}
		if (phase < 0 || advance > 2)
			return V32_SYMBOL_TIMING_INVALID_STATE;
		next.polyphase_index = phase;
		next.samples_until_output = advance;
		recovered[produced++] = symbol;
	}
	if (!v32_symbol_timing_is_valid(&next))
		return V32_SYMBOL_TIMING_INVALID_STATE;
	if (produced > output_capacity || (output == NULL && produced != 0))
		return V32_SYMBOL_TIMING_INVALID_ARGUMENT;
	if (memory_regions_overlap(output, produced * sizeof(*output),
				   timing, sizeof(*timing)) ||
	    memory_regions_overlap(output, produced * sizeof(*output),
				   input_start, count * sizeof(*input_start)) ||
	    memory_regions_overlap(output, produced * sizeof(*output),
				   output_count, sizeof(*output_count)))
		return V32_SYMBOL_TIMING_INVALID_ARGUMENT;
	if (produced != 0)
		memcpy(output, recovered, produced * sizeof(*output));
	*timing = next;
	*output_count = produced;
	return V32_SYMBOL_TIMING_OK;
}
