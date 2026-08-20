// SPDX-License-Identifier: BSD-3-Clause

#include <v34_carrier_demodulator.h>

#include "test.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct carrier_profile {
	uint32_t numerator;
	uint32_t denominator;
	uint32_t phase_step;
	uint32_t final_phase;
};

static const struct carrier_profile CARRIER_PROFILES[] = {
	{ 1600, 1, UINT32_C(0x33333333), UINT32_C(0x33333000) },
	{ 1800, 1, UINT32_C(0x3999999a), UINT32_C(0x9999a000) },
	{ 11520, 7, UINT32_C(0x34a9b101), UINT32_C(0x9b101000) },
	{ 12800, 7, UINT32_C(0x3a83a83b), UINT32_C(0x3a83b000) },
	{ 1680, 1, UINT32_C(0x35c28f5c), UINT32_C(0x28f5c000) },
	{ 5600, 3, UINT32_C(0x3bbbbbbc), UINT32_C(0xbbbbc000) },
	{ 2000, 1, UINT32_C(0x40000000), UINT32_C(0x00000000) },
	{ 1920, 1, UINT32_C(0x3d70a3d7), UINT32_C(0x0a3d7000) },
	{ 96000, 49, UINT32_C(0x3eb1a1f6), UINT32_C(0x1a1f6000) },
};

static uint64_t hash_word(uint64_t hash, uint16_t word)
{
	for (unsigned int byte = 0; byte < 2; ++byte) {
		hash ^= (word >> (byte * 8u)) & 0xffu;
		hash *= UINT64_C(1099511628211);
	}
	return hash;
}

static uint64_t hash_u32(uint64_t hash, uint32_t value)
{
	hash = hash_word(hash, (uint16_t)value);
	return hash_word(hash, (uint16_t)(value >> 16));
}

static bool test_exact_profiles(void)
{
	uint64_t hash = UINT64_C(1469598103934665603);
	uint32_t random = UINT32_C(0xd1b54a32);

	for (size_t profile_index = 0;
	     profile_index < ARRAY_LENGTH(CARRIER_PROFILES);
	     ++profile_index) {
		const struct carrier_profile *profile =
			&CARRIER_PROFILES[profile_index];
		struct v34_carrier_demodulator demodulator;

		CHECK(v34_carrier_demodulator_init(
			      &demodulator, 8000, profile->numerator,
			      profile->denominator) ==
			      V34_CARRIER_DEMODULATOR_OK &&
			      demodulator.phase == 0 &&
			      demodulator.phase_step == profile->phase_step,
		      "V.34 carrier profile %zu did not initialize",
		      profile_index);
		hash = hash_u32(hash, demodulator.phase_step);
		for (unsigned int index = 0; index < 4096; ++index) {
			struct v34_analytic_sample input;
			struct v34_analytic_sample output;

			random = random * UINT32_C(1664525) +
				 UINT32_C(1013904223);
			input.in_phase = (int16_t)(random >> 16);
			random = random * UINT32_C(1664525) +
				 UINT32_C(1013904223);
			input.quadrature = (int16_t)(random >> 16);
			CHECK(v34_carrier_demodulator_process(
				      &demodulator, input, &output) ==
				      V34_CARRIER_DEMODULATOR_OK,
			      "V.34 carrier profile %zu failed at sample %u",
			      profile_index, index);
			hash = hash_word(hash, (uint16_t)output.in_phase);
			hash = hash_word(hash, (uint16_t)output.quadrature);
		}
		CHECK(demodulator.phase == profile->final_phase,
		      "V.34 carrier profile %zu phase changed: %#x",
		      profile_index, demodulator.phase);
		hash = hash_u32(hash, demodulator.phase);
	}
	CHECK(random == UINT32_C(0x233fea32) &&
		      hash == UINT64_C(0xa160998f6f309c73),
	      "V.34 carrier oracle changed: %#x/%#llx", random,
	      (unsigned long long)hash);
	return true;
}

static bool test_ownership(void)
{
	struct v34_carrier_demodulator left;
	struct v34_carrier_demodulator right;
	struct v34_carrier_demodulator saved;
	struct v34_carrier_demodulator uninitialized = { 0 };
	struct v34_analytic_sample output = { 123, -456 };
	struct v34_analytic_sample saved_output = output;
	uint32_t random = UINT32_C(0xa341316c);

	CHECK(v34_carrier_demodulator_init(&left, 8000, 1800, 1) ==
		      V34_CARRIER_DEMODULATOR_OK &&
		      v34_carrier_demodulator_init(&right, 8000, 1800, 1) ==
			      V34_CARRIER_DEMODULATOR_OK &&
		      !v34_carrier_demodulator_is_valid(NULL) &&
		      !v34_carrier_demodulator_is_valid(&uninitialized),
	      "V.34 carrier lifecycle changed");
	CHECK(v34_carrier_demodulator_init(NULL, 8000, 1800, 1) ==
		      V34_CARRIER_DEMODULATOR_INVALID_ARGUMENT &&
		      v34_carrier_demodulator_init(
			      &uninitialized, 0, 1800, 1) ==
			      V34_CARRIER_DEMODULATOR_INVALID_ARGUMENT &&
		      v34_carrier_demodulator_init(
			      &uninitialized, 8000, 0, 1) ==
			      V34_CARRIER_DEMODULATOR_INVALID_ARGUMENT &&
		      v34_carrier_demodulator_init(
			      &uninitialized, 8000, 1800, 0) ==
			      V34_CARRIER_DEMODULATOR_INVALID_ARGUMENT &&
		      v34_carrier_demodulator_init(
			      &uninitialized, 8000, 4000, 1) ==
			      V34_CARRIER_DEMODULATOR_INVALID_ARGUMENT &&
		      !v34_carrier_demodulator_is_valid(&uninitialized),
	      "V.34 carrier initialization rejection changed");
	saved = left;
	CHECK(v34_carrier_demodulator_process(
		      NULL, (struct v34_analytic_sample){ 0, 0 }, &output) ==
		      V34_CARRIER_DEMODULATOR_INVALID_STATE &&
		      v34_carrier_demodulator_process(
			      &uninitialized,
			      (struct v34_analytic_sample){ 0, 0 }, &output) ==
			      V34_CARRIER_DEMODULATOR_INVALID_STATE &&
		      v34_carrier_demodulator_process(
			      &left, (struct v34_analytic_sample){ 0, 0 }, NULL) ==
			      V34_CARRIER_DEMODULATOR_INVALID_ARGUMENT &&
		      v34_carrier_demodulator_process(
			      &left, (struct v34_analytic_sample){ 0, 0 },
			      (struct v34_analytic_sample *)&left) ==
			      V34_CARRIER_DEMODULATOR_INVALID_ARGUMENT &&
		      memcmp(&left, &saved, sizeof(left)) == 0 &&
		      memcmp(&output, &saved_output, sizeof(output)) == 0,
	      "V.34 carrier rejection was not transactional");

	for (unsigned int index = 0; index < 512; ++index) {
		struct v34_analytic_sample input;
		struct v34_analytic_sample left_output;
		struct v34_analytic_sample right_output;

		random = random * UINT32_C(1103515245) + UINT32_C(12345);
		input.in_phase = (int16_t)(random >> 16);
		random = random * UINT32_C(1103515245) + UINT32_C(12345);
		input.quadrature = (int16_t)(random >> 16);
		CHECK(v34_carrier_demodulator_process(
			      &left, input, &left_output) ==
			      V34_CARRIER_DEMODULATOR_OK &&
			      v34_carrier_demodulator_process(
				      &right, input, &right_output) ==
				      V34_CARRIER_DEMODULATOR_OK &&
			      memcmp(&left, &right, sizeof(left)) == 0 &&
			      memcmp(&left_output, &right_output,
				     sizeof(left_output)) == 0,
		      "V.34 carrier owners diverged at sample %u", index);
	}
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "exact profiles", test_exact_profiles },
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
