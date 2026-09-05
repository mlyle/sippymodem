// SPDX-License-Identifier: BSD-3-Clause

#include <agc.h>
#include <v34_gain_control.h>

#include "test.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static uint64_t hash_word(uint64_t hash, uint16_t word)
{
	for (unsigned int byte = 0; byte < 2; ++byte) {
		hash ^= (word >> (byte * 8u)) & 0xffu;
		hash *= UINT64_C(1099511628211);
	}
	return hash;
}

static uint64_t hash_state(const struct v34_gain_control *control)
{
	uint64_t hash = UINT64_C(1469598103934665603);

	hash = hash_word(hash, control->energy_count);
	hash = hash_word(
		hash, (uint16_t)(control->energy_accumulator & 0xffffu));
	hash = hash_word(
		hash, (uint16_t)(control->energy_accumulator >> 16));
	hash = hash_word(hash, control->average_energy);
	hash = hash_word(hash, (uint16_t)control->gain);
	hash = hash_word(hash, (uint16_t)control->correction);
	/* Include the fixed correction coefficient from the canonical state. */
	hash = hash_word(hash, UINT16_C(0x3333));
	hash = hash_word(hash, control->history_index);
	for (size_t index = 0; index < V34_GAIN_HISTORY_SAMPLES; ++index)
		hash = hash_word(hash, (uint16_t)control->history[index]);
	return hash;
}

static bool test_exact_profile(void)
{
	struct v34_gain_control control;
	uint64_t output_hash = UINT64_C(1469598103934665603);
	uint32_t random = UINT32_C(0x6c41a793);
	uint16_t frozen_average = 0;
	int16_t frozen_gain = 0;
	int16_t frozen_correction = 0;

	CHECK(v34_gain_control_init(&control, 0x0600) ==
		      V34_GAIN_CONTROL_OK,
	      "V.34 gain owner did not initialize");
	for (unsigned int index = 0; index < 8192; ++index) {
		struct v34_analytic_sample input;
		struct v34_analytic_sample output;

		random = random * UINT32_C(1664525) + UINT32_C(1013904223);
		input.in_phase = (int16_t)(random >> 16);
		random = random * UINT32_C(1664525) + UINT32_C(1013904223);
		input.quadrature = (int16_t)(random >> 16);
		if (index < 2049) {
			input.in_phase = (int16_t)(input.in_phase / 8);
			input.quadrature = (int16_t)(input.quadrature / 8);
		} else if (index >= 6144) {
			input.in_phase = (int16_t)(input.in_phase / 4);
			input.quadrature = (int16_t)(input.quadrature / 4);
		}
		if (index == 2049) {
			frozen_average = control.average_energy;
			frozen_gain = control.gain;
			frozen_correction = control.correction;
			CHECK(frozen_average == UINT16_C(0x0d77) &&
				      frozen_gain == 1205 &&
				      frozen_correction == 0 &&
				      control.energy_count == 1 &&
				      control.energy_accumulator ==
					      UINT32_C(0x009c0e04) &&
				      control.history_index == 33 &&
				      v34_gain_control_set_mode(
					      &control, V34_GAIN_FROZEN) ==
					      V34_GAIN_CONTROL_OK,
			      "V.34 gain freeze boundary changed");
		} else if (index == 3075) {
			CHECK(control.average_energy == frozen_average &&
				      control.gain == frozen_gain &&
				      control.correction == frozen_correction &&
				      control.energy_count == 3 &&
				      control.energy_accumulator ==
					      UINT32_C(0x8258f050) &&
				      control.history_index == 15 &&
				      v34_gain_control_set_mode(
					      &control, V34_GAIN_TRACKING) ==
					      V34_GAIN_CONTROL_OK,
			      "V.34 frozen gain lineage changed");
		}
		CHECK(v34_gain_control_process(&control, input, &output) ==
			      V34_GAIN_CONTROL_OK,
		      "V.34 gain sample %u failed", index);
		output_hash = hash_word(output_hash, (uint16_t)output.in_phase);
		output_hash = hash_word(
			output_hash, (uint16_t)output.quadrature);
	}
	CHECK(random == UINT32_C(0xf0cde793) &&
		      output_hash == UINT64_C(0x86f1b50893004f3a) &&
		      hash_state(&control) == UINT64_C(0x730681e710c816ab) &&
		      control.energy_count == 0 &&
		      control.energy_accumulator == 0 &&
		      control.average_energy == UINT16_C(0x12fe) &&
		      control.gain == 743 && control.correction == -254 &&
		      control.history_index == 20,
	      "V.34 gain profile changed: %#x/%#llx/%#llx", random,
	      (unsigned long long)output_hash,
	      (unsigned long long)hash_state(&control));
	return true;
}

static bool test_saturation(void)
{
	struct v34_gain_control control;
	struct v34_analytic_sample output;

	CHECK(v34_gain_control_init(&control, INT16_MAX) ==
		      V34_GAIN_CONTROL_OK &&
		      v34_gain_control_process(
			      &control,
			      (struct v34_analytic_sample){
				      .in_phase = INT16_MAX,
				      .quadrature = INT16_MIN,
			      },
			      &output) == V34_GAIN_CONTROL_OK &&
		      output.in_phase == 32512 &&
		      output.quadrature == -32512,
	      "V.34 gain saturation changed: %d/%d",
	      output.in_phase, output.quadrature);
	return true;
}

static bool test_ownership(void)
{
	struct v34_gain_control left;
	struct v34_gain_control right;
	struct v34_gain_control saved;
	struct v34_gain_control uninitialized = { 0 };
	struct v34_analytic_sample output = { 123, -456 };
	struct v34_analytic_sample saved_output = output;
	uint32_t random = UINT32_C(0x4f1bbcdc);

	CHECK(v34_gain_control_init(&left, 0x0600) ==
		      V34_GAIN_CONTROL_OK &&
		      v34_gain_control_init(&right, 0x0600) ==
			      V34_GAIN_CONTROL_OK &&
		      !v34_gain_control_is_valid(NULL) &&
		      !v34_gain_control_is_valid(&uninitialized) &&
		      v34_gain_control_init(NULL, 0x0600) ==
			      V34_GAIN_CONTROL_INVALID_ARGUMENT &&
		      v34_gain_control_init(&uninitialized, 0) ==
			      V34_GAIN_CONTROL_INVALID_ARGUMENT &&
		      v34_gain_control_init(&uninitialized, -1) ==
			      V34_GAIN_CONTROL_INVALID_ARGUMENT,
	      "V.34 gain lifecycle contract changed");
	saved = left;
	CHECK(v34_gain_control_process(
		      NULL, (struct v34_analytic_sample){ 0, 0 }, &output) ==
		      V34_GAIN_CONTROL_INVALID_STATE &&
		      v34_gain_control_process(
			      &uninitialized,
			      (struct v34_analytic_sample){ 0, 0 }, &output) ==
			      V34_GAIN_CONTROL_INVALID_STATE &&
		      v34_gain_control_process(
			      &left, (struct v34_analytic_sample){ 0, 0 }, NULL) ==
			      V34_GAIN_CONTROL_INVALID_ARGUMENT &&
		      v34_gain_control_process(
			      &left, (struct v34_analytic_sample){ 0, 0 },
			      (struct v34_analytic_sample *)&left) ==
			      V34_GAIN_CONTROL_INVALID_ARGUMENT &&
		      v34_gain_control_set_mode(
			      &left, (enum v34_gain_mode)2) ==
			      V34_GAIN_CONTROL_INVALID_ARGUMENT &&
		      memcmp(&left, &saved, sizeof(left)) == 0 &&
		      memcmp(&output, &saved_output, sizeof(output)) == 0,
	      "V.34 gain rejection was not transactional");

	for (unsigned int index = 0; index < 512; ++index) {
		struct v34_analytic_sample input;
		struct v34_analytic_sample left_output;
		struct v34_analytic_sample right_output;

		random = random * UINT32_C(1103515245) + UINT32_C(12345);
		input.in_phase = (int16_t)(random >> 16);
		random = random * UINT32_C(1103515245) + UINT32_C(12345);
		input.quadrature = (int16_t)(random >> 16);
		CHECK(v34_gain_control_process(
			      &left, input, &left_output) ==
			      V34_GAIN_CONTROL_OK &&
			      v34_gain_control_process(
				      &right, input, &right_output) ==
				      V34_GAIN_CONTROL_OK &&
			      memcmp(&left_output, &right_output,
				     sizeof(left_output)) == 0 &&
			      memcmp(&left, &right, sizeof(left)) == 0,
		      "V.34 gain owners diverged at sample %u", index);
	}
	return true;
}

static bool test_measured_refresh(void)
{
	static const struct {
		int16_t sample;
		bool alternating;
		unsigned int nonzero_samples;
		int16_t level;
		int16_t initial_gain;
		int16_t refreshed_gain;
	} cases[] = {
		{ 0, false, 36, 0, 468, 468 },
		{ 8, false, 36, 0, 468, 468 },
		{ 37, false, 26, 31, 468, 468 },
		{ 37, false, 28, 32, 468, 27136 },
		{ 100, false, 36, 85, 468, 27136 },
		{ 129, false, 36, 118, 468, 27136 },
		{ 132, false, 36, 119, 468, 26979 },
		/* The owned target RMS is sqrt(9,830,000). Dividing its Q10
		 * value by these measured levels rounds up and down respectively. */
		{ 347, false, 36, 335, 468, 9584 },
		{ 4063, false, 36, 4047, 468, 793 },
		{ 1000, false, 36, 985, 468, 3259 },
		{ 347, false, 36, 335, 0, 9584 },
		{ -347, false, 36, 353, 468, 9095 },
		{ 347, true, 36, 344, 468, 9333 },
		/* Adjacent Q10 gains straddle the lower energy deadband. */
		{ 1000, false, 36, 985, 2727, 3259 },
		{ 1000, false, 36, 985, 2728, 2728 },
		{ 8000, false, 36, 7997, 335, 401 },
		{ 8000, false, 36, 7997, 336, 336 },
		/* The current gain is inside, or above, the energy deadband. */
		{ 8000, false, 36, 7997, 400, 400 },
		{ 8000, false, 36, 7997, 1000, 1000 },
		{ 37, false, 36, 36, 27136, 27136 },
		{ 37, false, 36, 36, 30000, 30000 },
		{ INT16_MIN, false, 36, 32703, 468, 468 },
		{ INT16_MAX, true, 36, 32703, 0, 98 },
	};

	for (size_t index = 0; index < ARRAY_LENGTH(cases); ++index) {
		struct v34_gain_control control;
		struct v34_gain_control expected;
		int16_t level = -1;

		CHECK(v34_gain_control_init(&control, 468) ==
			      V34_GAIN_CONTROL_OK &&
		      v34_gain_control_set_mode(&control, V34_GAIN_FROZEN) ==
			      V34_GAIN_CONTROL_OK,
		      "refresh case %zu did not initialize", index);
		for (unsigned int count = 0; count < V34_GAIN_HISTORY_SAMPLES;
		     ++count) {
			struct v34_analytic_sample output;
			struct v34_analytic_sample input = {
				.in_phase = count < cases[index].nonzero_samples ?
					cases[index].sample : 0,
			};

			if (cases[index].alternating && (count & 1u) != 0u)
				input.in_phase = (int16_t)-input.in_phase;
			CHECK(v34_gain_control_process(&control, input, &output) ==
				      V34_GAIN_CONTROL_OK,
			      "refresh case %zu could not fill history", index);
		}
		CHECK(agc_measure_level_s16(control.history,
			      V34_GAIN_HISTORY_SAMPLES, &level) == AGC_OK &&
		      level == cases[index].level,
		      "refresh case %zu has level %d, expected %d",
		      index, level, cases[index].level);
		/* Refresh only the gain, including while a correction is partly
		 * accumulated. The running correction owner keeps its lineage. */
		control.energy_count = 2;
		control.energy_accumulator = 329981;
		control.average_energy = 1200;
		control.correction = -173;
		for (unsigned int mode = 0; mode < 2u; ++mode) {
			control.gain = cases[index].initial_gain;
			control.mode = mode == 0u ? V34_GAIN_FROZEN : V34_GAIN_TRACKING;
			expected = control;
			expected.gain = cases[index].refreshed_gain;
			CHECK(v34_gain_control_refresh_level(&control) ==
				      V34_GAIN_CONTROL_OK &&
			      memcmp(&control, &expected, sizeof(control)) == 0 &&
			      v34_gain_control_refresh_level(&control) ==
				      V34_GAIN_CONTROL_OK &&
			      memcmp(&control, &expected, sizeof(control)) == 0,
			      "refresh case %zu mode %u gave gain %d, expected %d",
			      index, mode, control.gain, expected.gain);
		}
	}
	{
		struct v34_gain_control invalid = { 0 };
		struct v34_gain_control saved = invalid;

		CHECK(v34_gain_control_refresh_level(NULL) ==
			      V34_GAIN_CONTROL_INVALID_STATE &&
		      v34_gain_control_refresh_level(&invalid) ==
			      V34_GAIN_CONTROL_INVALID_STATE &&
		      memcmp(&invalid, &saved, sizeof(invalid)) == 0 &&
		      v34_gain_control_init(&invalid, 468) == V34_GAIN_CONTROL_OK,
		      "refresh accepted an uninitialized owner");
		invalid.history_index = V34_GAIN_HISTORY_SAMPLES;
		saved = invalid;
		CHECK(v34_gain_control_refresh_level(&invalid) ==
			      V34_GAIN_CONTROL_INVALID_STATE &&
		      memcmp(&invalid, &saved, sizeof(invalid)) == 0,
		      "invalid refresh history changed state");
	}
	return true;
}

static bool test_refresh_continuation(void)
{
	static const uint16_t expected_energy[] = { 1503, 1342, 1182 };
	const struct v34_analytic_sample input = { .in_phase = 347 };

	for (unsigned int mode = 0; mode < 2u; ++mode)
		for (uint16_t consumed = 1; consumed < V34_GAIN_UPDATE_SAMPLES;
		     ++consumed) {
			struct v34_gain_control control;
			struct v34_analytic_sample output;
			uint16_t remaining = V34_GAIN_UPDATE_SAMPLES - consumed;

			CHECK(v34_gain_control_init(&control, 468) ==
				      V34_GAIN_CONTROL_OK &&
			      v34_gain_control_set_mode(&control, V34_GAIN_FROZEN) ==
				      V34_GAIN_CONTROL_OK,
			      "continuation owner did not initialize");
			for (unsigned int count = 0; count < V34_GAIN_HISTORY_SAMPLES;
			     ++count)
				CHECK(v34_gain_control_process(&control, input, &output) ==
					      V34_GAIN_CONTROL_OK,
				      "continuation history did not advance");
			control.mode = mode == 0u ? V34_GAIN_FROZEN : V34_GAIN_TRACKING;
			control.average_energy = 1200;
			control.correction = -173;
			control.energy_count = consumed;
			/* The first samples were actually emitted with the old gain;
			 * preserve their 159^2 energy, not a rescaled fiction. */
			control.energy_accumulator = consumed * UINT32_C(159) * 159u;
			CHECK(v34_gain_control_refresh_level(&control) ==
				      V34_GAIN_CONTROL_OK && control.gain == 9584,
			      "continuation gain did not refresh");
			for (uint16_t count = 0; count < remaining; ++count) {
				CHECK(v34_gain_control_process(&control, input, &output) ==
					      V34_GAIN_CONTROL_OK &&
				      output.in_phase == 3248 && output.quadrature == 0,
				      "continuation output changed before correction");
				if (count + 1u < remaining)
					CHECK(control.gain == 9584 &&
					      control.energy_count == consumed + count + 1u &&
					      control.energy_accumulator ==
						consumed * UINT32_C(159) * 159u +
						(count + 1u) * UINT32_C(3248) * 3248u &&
					      control.average_energy == 1200 &&
					      control.correction == -173,
					      "partial mixed-gain energy was reset or rescaled");
			}
			CHECK(control.energy_count == 0u &&
			      control.energy_accumulator == 0u &&
			      control.history_index == remaining &&
			      control.average_energy == (mode == 0u ?
				      1200 : expected_energy[consumed - 1u]) &&
			      control.gain == (mode == 0u ? 9584 : 10753) &&
			      control.correction == (mode == 0u ? -173 : 0),
			      "refresh continuation changed the existing correction clock");
		}
	return true;
}

static bool test_info_profile(void)
{
	struct v34_gain_control control;
	int16_t output;

	CHECK(v34_gain_control_init(&control, 0x0400) ==
		      V34_GAIN_CONTROL_OK,
	      "INFO gain owner did not initialize");
	for (unsigned int index = 0; index < 8; ++index) {
		CHECK(v34_gain_control_process_info(&control, 4096, &output) ==
			      V34_GAIN_CONTROL_OK && output == 4096 &&
			      control.history_index == index + 1u &&
			      control.energy_count == (index + 1u) % 4u,
		      "INFO gain changed the four-sample correction boundary");
		if (index == 3)
			CHECK(control.average_energy == 1024 &&
				      control.correction == -298 && control.gain == 1024,
			      "INFO correction did not retain its slower first step");
	}
	CHECK(control.average_energy == 1894 &&
		      control.correction == 0 && control.gain == 1148,
	      "INFO correction did not use the second four-sample block");
	CHECK(v34_gain_control_set_mode(&control, V34_GAIN_FROZEN) ==
		      V34_GAIN_CONTROL_OK,
	      "INFO gain owner did not freeze");
	for (unsigned int index = 0; index < 6; ++index)
		CHECK(v34_gain_control_process_info(&control, 2048, &output) ==
			      V34_GAIN_CONTROL_OK && output == 2296 &&
			      control.gain == 1148 && control.average_energy == 1894 &&
			      control.correction == 0 &&
			      control.history_index == index + 9u &&
			      control.energy_count == (index + 1u) % 4u,
		      "frozen INFO gain changed learned state or stopped history");
	CHECK(v34_gain_control_init(&control, 0x0600) ==
		      V34_GAIN_CONTROL_OK &&
		      v34_gain_control_process_info(&control, -1, &output) ==
			      V34_GAIN_CONTROL_OK && output == -2 &&
		      v34_gain_control_process_info(&control, 1, &output) ==
			      V34_GAIN_CONTROL_OK && output == 1,
	      "INFO Q10 scaling did not truncate signed samples");
	for (unsigned int index = 0; index < 400; ++index)
		CHECK(v34_gain_control_process_info(&control, 8, &output) ==
			      V34_GAIN_CONTROL_OK && output == 12 &&
			      control.gain == 0x0600 && control.average_energy == 0 &&
			      control.correction == 0,
		      "quiet INFO input adapted its gain");
	CHECK(v34_gain_control_init(&control, INT16_MAX) ==
		      V34_GAIN_CONTROL_OK &&
		      v34_gain_control_process_info(&control, INT16_MAX, &output) ==
			      V34_GAIN_CONTROL_OK && output == 0x7f00 &&
		      v34_gain_control_process_info(&control, INT16_MIN, &output) ==
			      V34_GAIN_CONTROL_OK && output == (int16_t)0x8100,
	      "INFO saturation changed");
	return true;
}

static bool test_info_ownership(void)
{
	struct v34_gain_control control = { 0 };
	struct v34_gain_control snapshot = control;
	int16_t output = 137;

	CHECK(v34_gain_control_process_info(NULL, 2048, &output) ==
		      V34_GAIN_CONTROL_INVALID_STATE && output == 137 &&
		      v34_gain_control_process_info(&control, 2048, &output) ==
			      V34_GAIN_CONTROL_INVALID_STATE &&
		      memcmp(&control, &snapshot, sizeof(control)) == 0 && output == 137,
	      "uninitialized INFO gain mutated state or output");
	CHECK(v34_gain_control_init(&control, 0x0600) ==
		      V34_GAIN_CONTROL_OK,
	      "INFO ownership fixture did not initialize");
	snapshot = control;
	CHECK(v34_gain_control_process_info(&control, 2048, NULL) ==
		      V34_GAIN_CONTROL_INVALID_ARGUMENT &&
		      memcmp(&control, &snapshot, sizeof(control)) == 0 &&
		      v34_gain_control_process_info(&control, 2048, &control.gain) ==
			      V34_GAIN_CONTROL_INVALID_ARGUMENT &&
		      memcmp(&control, &snapshot, sizeof(control)) == 0 &&
		      v34_gain_control_process_info(&control, 2048,
			      &control.history[35]) == V34_GAIN_CONTROL_INVALID_ARGUMENT &&
		      memcmp(&control, &snapshot, sizeof(control)) == 0,
	      "INFO gain accepted an absent or aliased output");
	control.energy_count = V34_GAIN_UPDATE_SAMPLES;
	snapshot = control;
	CHECK(v34_gain_control_process_info(&control, 2048, &output) ==
		      V34_GAIN_CONTROL_INVALID_STATE &&
		      memcmp(&control, &snapshot, sizeof(control)) == 0 && output == 137,
	      "invalid INFO gain mutated state or output");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "exact profile", test_exact_profile },
		{ "INFO scalar profile", test_info_profile },
		{ "INFO scalar ownership", test_info_ownership },
		{ "saturation", test_saturation },
		{ "ownership", test_ownership },
		{ "measured acquisition refresh", test_measured_refresh },
		{ "refresh correction continuation", test_refresh_continuation },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		if (!tests[index].run()) {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failures++;
		} else {
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		}
	}
	return failures == 0 ? 0 : 1;
}
