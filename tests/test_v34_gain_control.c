// SPDX-License-Identifier: BSD-3-Clause

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

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "exact profile", test_exact_profile },
		{ "saturation", test_saturation },
		{ "ownership", test_ownership },
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
