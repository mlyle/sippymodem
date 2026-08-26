// SPDX-License-Identifier: BSD-3-Clause

#include <agc.h>
#include <integer_math.h>

#include "test.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
	AGC_TEST_SAMPLES = 36,
};

static const struct agc_config TEST_CONFIG = {
	.target_level_q15 = 0x2365,
	.acquisition_threshold = 0x0002,
	.tracking_threshold = 0x0050,
	.block_samples = AGC_TEST_SAMPLES,
	.history_weight_q15 = 0x4000,
	.input_weight_q15 = 0x4000,
};

static uint64_t hash_i16(const int16_t *samples, size_t count)
{
	uint64_t hash = UINT64_C(14695981039346656037);

	for (size_t index = 0; index < count; ++index) {
		uint16_t word = (uint16_t)samples[index];

		hash ^= word & UINT16_C(0xff);
		hash *= UINT64_C(1099511628211);
		hash ^= word >> 8;
		hash *= UINT64_C(1099511628211);
	}
	return hash;
}

static void fill_reference_block(int16_t *samples, unsigned seed)
{
	for (size_t index = 0; index < AGC_TEST_SAMPLES; ++index) {
		unsigned value = (unsigned)index * 977u + seed * 321u;

		samples[index] = (int16_t)(value % 12000u) - 6000;
	}
}

static bool test_fixed_square_root_profile(void)
{
	uint32_t value = 1;
	uint64_t hash = UINT64_C(1469598103934665603);

	CHECK(fixed_square_root_u32(0) == 0 &&
	      fixed_square_root_u32(1) == 1 &&
	      fixed_square_root_u32(UINT32_C(0x40000000)) == 32703 &&
	      fixed_square_root_u32(UINT32_MAX) == 32703,
	      "fixed square-root boundaries changed");
	for (size_t index = 0; index < 65536; ++index) {
		uint16_t root = fixed_square_root_u32(value);

		hash = (hash ^ (root & UINT16_C(0xff))) *
		       UINT64_C(1099511628211);
		hash = (hash ^ (root >> 8)) * UINT64_C(1099511628211);
		value = value * UINT32_C(1664525) + UINT32_C(1013904223);
	}
	CHECK(hash == UINT64_C(0xcb8e125169ce8fe8),
	      "fixed square-root profile changed");
	return true;
}

static bool test_exact_arithmetic(void)
{
	static const struct {
		int16_t input;
		int16_t smoothed;
		int16_t mantissa;
		int16_t shift;
		int16_t output;
	} constant_cases[] = {
		{ 4000, 1999, 4639, 4, 9056 },
		{ 4252, 2126, 8720, 3, 9048 },
		{ 6000, 2996, 6202, 3, 9080 },
		{ 12000, 5992, 6202, 2, 9084 },
	};
	struct agc control;
	int16_t input[AGC_TEST_SAMPLES];
	int16_t output[AGC_TEST_SAMPLES];

	for (size_t test = 0; test < ARRAY_LENGTH(constant_cases); ++test) {
		for (size_t index = 0; index < ARRAY_LENGTH(input); ++index)
			input[index] = constant_cases[test].input;
		CHECK(agc_init(&control, &TEST_CONFIG) == AGC_OK &&
		      agc_process(&control, input, ARRAY_LENGTH(input), output) ==
			      AGC_OK,
		      "AGC rejected constant case %zu", test);
		CHECK(control.smoothed_level == constant_cases[test].smoothed &&
		      control.gain_mantissa_q15 ==
			      constant_cases[test].mantissa &&
		      control.gain_shift == constant_cases[test].shift &&
		      control.signal_present,
		      "AGC state mismatch for input %d: got "
		      "smoothed=%d mantissa=%d shift=%d",
		      constant_cases[test].input, control.smoothed_level,
		      control.gain_mantissa_q15, control.gain_shift);
		for (size_t index = 0; index < ARRAY_LENGTH(output); ++index)
			CHECK(output[index] == constant_cases[test].output,
			      "AGC output mismatch for input %d",
			      constant_cases[test].input);
	}

	/* Independent fixed-arithmetic oracle over a nonconstant block. */
	fill_reference_block(input, 1);
	CHECK(hash_i16(input, ARRAY_LENGTH(input)) ==
		      UINT64_C(0xf8de583aed8368e4),
	      "AGC oracle input changed");
	CHECK(agc_init(&control, &TEST_CONFIG) == AGC_OK &&
	      agc_process(&control, input, ARRAY_LENGTH(input), output) == AGC_OK,
	      "AGC rejected oracle block");
	CHECK(hash_i16(output, ARRAY_LENGTH(output)) ==
		      UINT64_C(0x5f0c022247850619) &&
	      control.smoothed_level == 1702 &&
	      control.gain_mantissa_q15 == 5444 &&
	      control.gain_shift == 4 && control.signal_present,
	      "AGC nonconstant oracle mismatch");

	for (size_t index = 0; index < ARRAY_LENGTH(input); ++index)
		input[index] = 4252;
	CHECK(agc_init(&control, &TEST_CONFIG) == AGC_OK,
	      "AGC boundary owner did not initialize");
	control.smoothed_level = 3939;
	control.gain_mantissa_q15 = 4714;
	control.gain_shift = 3;
	CHECK(agc_process(&control, input, ARRAY_LENGTH(input), output) == AGC_OK &&
	      control.smoothed_level == 4095 &&
	      control.gain_mantissa_q15 == 4530 &&
	      control.gain_shift == 3 && output[0] == 4696,
	      "AGC reciprocal endpoint mismatch");
	return true;
}

static bool test_lifecycle_and_contracts(void)
{
	struct agc control;
	struct agc saved;
	struct agc_config invalid = TEST_CONFIG;
	int16_t input[AGC_TEST_SAMPLES];
	int16_t output[AGC_TEST_SAMPLES];
	int16_t saved_output[AGC_TEST_SAMPLES];
	int16_t overlap[AGC_TEST_SAMPLES + 1u];
	int16_t saved_overlap[AGC_TEST_SAMPLES + 1u];
	int16_t trained_mantissa;
	int16_t trained_shift;

	for (size_t index = 0; index < ARRAY_LENGTH(input); ++index)
		input[index] = 4252;
	memset(&control, 0x5a, sizeof(control));
	saved = control;
	invalid.input_weight_q15--;
	CHECK(agc_init(&control, &invalid) == AGC_INVALID_ARGUMENT &&
	      memcmp(&control, &saved, sizeof(control)) == 0 &&
	      agc_init(NULL, &TEST_CONFIG) == AGC_INVALID_ARGUMENT,
	      "AGC invalid initialization changed its owner");
	CHECK(agc_init(&control, &TEST_CONFIG) == AGC_OK &&
	      agc_process(&control, input, ARRAY_LENGTH(input), output) == AGC_OK,
	      "AGC lifecycle setup failed");
	trained_mantissa = control.gain_mantissa_q15;
	trained_shift = control.gain_shift;
	CHECK(agc_freeze(&control) == AGC_OK, "AGC did not freeze");
	for (size_t index = 0; index < ARRAY_LENGTH(input); ++index)
		input[index] = 6000;
	CHECK(agc_process(&control, input, ARRAY_LENGTH(input), output) == AGC_OK &&
	      control.gain_mantissa_q15 == trained_mantissa &&
	      control.gain_shift == trained_shift,
	      "frozen AGC changed trained gain");
	CHECK(agc_reacquire(&control) == AGC_OK &&
	      control.gain_mode == AGC_GAIN_TRACKING &&
	      control.smoothed_level == 0 && !control.signal_present &&
	      control.gain_mantissa_q15 == trained_mantissa &&
	      control.gain_shift == trained_shift,
	      "AGC reacquire did not preserve only trained gain");
	fill_reference_block(input, 7);
	CHECK(agc_init(&control, &TEST_CONFIG) == AGC_OK &&
	      agc_process(&control, input, ARRAY_LENGTH(input), input) == AGC_OK &&
	      hash_i16(input, ARRAY_LENGTH(input)) ==
		      UINT64_C(0x694a1a6d1a7a33e2) &&
	      control.smoothed_level == 1717 &&
	      control.gain_mantissa_q15 == 5394 &&
	      control.gain_shift == 4,
	      "AGC exact in-place processing diverged from its oracle");

	memset(output, 0x39, sizeof(output));
	memcpy(saved_output, output, sizeof(output));
	saved = control;
	CHECK(agc_process(&control, input, ARRAY_LENGTH(input) - 1u, output) ==
		      AGC_INVALID_ARGUMENT &&
	      memcmp(&control, &saved, sizeof(control)) == 0 &&
	      memcmp(output, saved_output, sizeof(output)) == 0,
	      "AGC count rejection was not transactional");
	CHECK(agc_process(
		      &control, input, ARRAY_LENGTH(input),
		      (int16_t *)(void *)&control) == AGC_INVALID_ARGUMENT &&
	      memcmp(&control, &saved, sizeof(control)) == 0,
	      "AGC owner alias was accepted");
	fill_reference_block(overlap, 19);
	overlap[ARRAY_LENGTH(overlap) - 1u] = 1234;
	memcpy(saved_overlap, overlap, sizeof(overlap));
	CHECK(agc_process(
		      &control, overlap, AGC_TEST_SAMPLES, &overlap[1]) ==
		      AGC_INVALID_ARGUMENT &&
	      agc_process(&control, NULL, AGC_TEST_SAMPLES, output) ==
		      AGC_INVALID_ARGUMENT &&
	      agc_process(&control, overlap, AGC_TEST_SAMPLES, NULL) ==
		      AGC_INVALID_ARGUMENT &&
	      memcmp(&control, &saved, sizeof(control)) == 0 &&
	      memcmp(overlap, saved_overlap, sizeof(overlap)) == 0 &&
	      memcmp(output, saved_output, sizeof(output)) == 0,
	      "AGC null/partial-overlap rejection was not transactional");
	control.gain_shift = -1;
	saved = control;
	CHECK(!agc_is_valid(&control) &&
	      agc_process(&control, input, ARRAY_LENGTH(input), output) ==
		      AGC_INVALID_STATE &&
	      memcmp(&control, &saved, sizeof(control)) == 0 &&
	      memcmp(output, saved_output, sizeof(output)) == 0,
	      "AGC accepted unreachable negative shift state");
	return true;
}

static bool test_level_measurement_contract(void)
{
	static const struct {
		int16_t input;
		int16_t level_36;
		int16_t level_9;
	} cases[] = {
		{ 4000, 3998, 1999 },
		{ 4252, 4252, 2126 },
		{ 6000, 5992, 2996 },
		{ 12000, 11985, 5992 },
	};
	int16_t samples[AGC_TEST_SAMPLES];
	int16_t saved[AGC_TEST_SAMPLES];
	int16_t level;

	for (size_t test = 0; test < ARRAY_LENGTH(cases); ++test) {
		for (size_t index = 0; index < ARRAY_LENGTH(samples); ++index)
			samples[index] = cases[test].input;
		CHECK(agc_measure_level_s16(
			      samples, ARRAY_LENGTH(samples), &level) == AGC_OK &&
		      level == cases[test].level_36 &&
		      agc_measure_level_s16(samples, 9, &level) == AGC_OK &&
		      level == cases[test].level_9,
		      "level measurement diverged for input %d",
		      cases[test].input);
	}

	fill_reference_block(samples, 1);
	CHECK(agc_measure_level_s16(
		      samples, ARRAY_LENGTH(samples), &level) == AGC_OK &&
	      level == 3405 &&
	      agc_measure_level_s16(samples, 9, &level) == AGC_OK &&
	      level == 1541,
	      "level measurement nonconstant oracle diverged");
	memcpy(saved, samples, sizeof(samples));
	level = 12345;
#define CHECK_LEVEL_REJECT(call, label)                                    \
	CHECK((call) == AGC_INVALID_ARGUMENT && level == 12345 &&             \
	      memcmp(samples, saved, sizeof(samples)) == 0,                    \
	      "%s was not transactional", label)
	CHECK_LEVEL_REJECT(agc_measure_level_s16(NULL, 9, &level),
			   "null level input");
	CHECK_LEVEL_REJECT(agc_measure_level_s16(samples, 0, &level),
			   "empty level input");
	CHECK_LEVEL_REJECT(agc_measure_level_s16(
			   samples, (size_t)UINT16_MAX + 1u, &level),
			   "oversize level input");
	CHECK_LEVEL_REJECT(agc_measure_level_s16(
			   samples, 9, (int16_t *)(void *)&samples[3]),
			   "overlapping level output");
	CHECK_LEVEL_REJECT(agc_measure_level_s16(samples, 9, NULL),
			   "null level output");
#undef CHECK_LEVEL_REJECT
	return true;
}

int main(void)
{
	if (!test_fixed_square_root_profile() || !test_exact_arithmetic() ||
	    !test_lifecycle_and_contracts() ||
	    !test_level_measurement_contract())
		return 1;
	puts("AGC tests passed");
	return 0;
}
