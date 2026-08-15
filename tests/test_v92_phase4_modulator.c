// SPDX-License-Identifier: BSD-3-Clause

#include <v92_phase4_modulator.h>

#include "test.h"

#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
	TEST_LU = 5000,
	GROUP_CYCLES = 24,
};

static const int16_t FOUR_POINT_MAGNITUDES[] = { 2236, 6708 };
static const int16_t EIGHT_POINT_MAGNITUDES[] = {
	1091, 3273, 5455, 7638,
};

static const int16_t *magnitudes(
	enum v92_constellation_size constellation, size_t *count)
{
	if (count == NULL)
		return NULL;
	if (constellation == V92_CONSTELLATION_4_POINT) {
		*count = ARRAY_LENGTH(FOUR_POINT_MAGNITUDES);
		return FOUR_POINT_MAGNITUDES;
	}
	if (constellation == V92_CONSTELLATION_8_POINT) {
		*count = ARRAY_LENGTH(EIGHT_POINT_MAGNITUDES);
		return EIGHT_POINT_MAGNITUDES;
	}
	return NULL;
}

static bool test_generated_levels(void)
{
	static const enum v92_constellation_size constellations[] = {
		V92_CONSTELLATION_4_POINT,
		V92_CONSTELLATION_8_POINT,
	};

	for (size_t selection = 0;
	     selection < ARRAY_LENGTH(constellations); ++selection) {
		size_t count = 0;
		const int16_t *expected = magnitudes(
			constellations[selection], &count);

		CHECK(expected != NULL &&
		      v92_phase4_modulator_bits_per_symbol(
			      constellations[selection]) == selection + 2u,
		      "V.92 Phase 4 bit width differed");
		for (size_t magnitude = 0; magnitude < count; ++magnitude) {
			for (uint8_t sign = 0; sign <= 1u; ++sign) {
				int16_t sample = 0;

				CHECK(v92_phase4_modulator_level(
					      constellations[selection], TEST_LU,
					      (uint8_t)magnitude, sign, &sample) ==
					      V92_PHASE4_MODULATOR_OK &&
				      sample == (sign != 0u ?
						 -expected[magnitude] :
						 expected[magnitude]),
				      "V.92 Phase 4 level differed for "
				      "%u points, %zu/%u",
				      (unsigned int)constellations[selection],
				      magnitude, sign);
			}
		}
	}
	return true;
}

static bool check_groups(
	enum v92_constellation_size constellation,
	uint8_t initial_previous_sign)
{
	struct v92_phase4_modulator modulator;
	struct v92_phase4_demodulator demodulator;
	unsigned int bits =
		v92_phase4_modulator_bits_per_symbol(constellation);
	uint8_t group_count = (uint8_t)(1u << bits);

	CHECK(v92_phase4_modulator_init(
		      &modulator, constellation, initial_previous_sign,
		      TEST_LU) == V92_PHASE4_MODULATOR_OK &&
	      v92_phase4_demodulator_init(
		      &demodulator, constellation, initial_previous_sign,
		      TEST_LU) == V92_PHASE4_MODULATOR_OK &&
	      v92_phase4_modulator_is_valid(&modulator) &&
	      v92_phase4_demodulator_is_valid(&demodulator),
	      "V.92 Phase 4 group state did not initialize");
	for (unsigned int cycle = 0; cycle < GROUP_CYCLES; ++cycle) {
		for (uint8_t input_group = 0;
		     input_group < group_count; ++input_group) {
			uint8_t recovered_group = 0xa5u;
			int16_t nearest_level = 1234;
			int16_t sample = 0;
			double noise = (cycle & 1u) != 0u ? 100.0 : -100.0;

			CHECK(v92_phase4_modulator_next(
				      &modulator, input_group, &sample) ==
				      V92_PHASE4_MODULATOR_OK &&
			      v92_phase4_demodulator_next(
				      &demodulator, sample + noise,
				      &recovered_group, &nearest_level) ==
				      V92_PHASE4_MODULATOR_OK,
			      "V.92 Phase 4 group %u/%u did not modulate",
			      cycle, input_group);
			CHECK(recovered_group == input_group &&
			      nearest_level == sample &&
			      modulator.previous_encoded_sign ==
				      demodulator.previous_encoded_sign &&
			      modulator.scrambler.history ==
				      demodulator.descrambler.history &&
			      v92_phase4_modulator_is_valid(&modulator) &&
			      v92_phase4_demodulator_is_valid(&demodulator),
			      "V.92 Phase 4 continuity differed at %u/%u",
			      cycle, input_group);
		}
	}
	return true;
}

static bool test_arbitrary_groups(void)
{
	static const enum v92_constellation_size constellations[] = {
		V92_CONSTELLATION_4_POINT,
		V92_CONSTELLATION_8_POINT,
	};

	for (size_t selection = 0;
	     selection < ARRAY_LENGTH(constellations); ++selection) {
		for (uint8_t previous_sign = 0;
		     previous_sign <= 1u; ++previous_sign)
			CHECK(check_groups(
				      constellations[selection], previous_sign),
			      "V.92 Phase 4 group sequence failed");
	}
	return true;
}

static bool test_contracts(void)
{
	struct v92_phase4_modulator modulator;
	struct v92_phase4_modulator before;
	struct v92_phase4_demodulator demodulator;
	struct v92_phase4_demodulator demodulator_before;
	uint8_t group = 0xa5u;
	int16_t nearest = 4321;
	int16_t sample = 1234;
	static const char *const names[] = {
		"ok", "invalid argument", "invalid state",
	};

	memset(&modulator, 0xa5, sizeof(modulator));
	before = modulator;
	CHECK(v92_phase4_modulator_bits_per_symbol(
		      (enum v92_constellation_size)6) == 0u &&
	      v92_phase4_modulator_init(
		      NULL, V92_CONSTELLATION_4_POINT, 0u, TEST_LU) ==
		      V92_PHASE4_MODULATOR_INVALID_ARGUMENT &&
	      v92_phase4_modulator_init(
		      &modulator, (enum v92_constellation_size)6, 0u,
		      TEST_LU) == V92_PHASE4_MODULATOR_INVALID_ARGUMENT &&
	      v92_phase4_modulator_init(
		      &modulator, V92_CONSTELLATION_4_POINT, 2u,
		      TEST_LU) == V92_PHASE4_MODULATOR_INVALID_ARGUMENT &&
	      v92_phase4_modulator_init(
		      &modulator, V92_CONSTELLATION_8_POINT, 0u,
		      INT16_MAX) == V92_PHASE4_MODULATOR_INVALID_ARGUMENT &&
	      memcmp(&modulator, &before, sizeof(modulator)) == 0,
	      "invalid V.92 Phase 4 initialization changed its destination");
	CHECK(v92_phase4_modulator_level(
		      V92_CONSTELLATION_4_POINT, TEST_LU, 2u, 0u,
		      &sample) == V92_PHASE4_MODULATOR_INVALID_ARGUMENT &&
	      v92_phase4_modulator_level(
		      V92_CONSTELLATION_8_POINT, TEST_LU, 0u, 2u,
		      &sample) == V92_PHASE4_MODULATOR_INVALID_ARGUMENT &&
	      v92_phase4_modulator_level(
		      V92_CONSTELLATION_8_POINT, INT16_MAX, 0u, 0u,
		      &sample) == V92_PHASE4_MODULATOR_INVALID_ARGUMENT &&
	      v92_phase4_modulator_level(
		      V92_CONSTELLATION_4_POINT, TEST_LU, 0u, 0u,
		      NULL) == V92_PHASE4_MODULATOR_INVALID_ARGUMENT &&
	      sample == 1234,
	      "invalid V.92 Phase 4 level request changed its output");
	CHECK(v92_phase4_modulator_init(
		      &modulator, V92_CONSTELLATION_4_POINT, 0u,
		      TEST_LU) == V92_PHASE4_MODULATOR_OK,
	      "V.92 Phase 4 contract state did not initialize");
	before = modulator;
	CHECK(!v92_phase4_modulator_is_valid(NULL) &&
	      v92_phase4_modulator_next(NULL, 0u, &sample) ==
		      V92_PHASE4_MODULATOR_INVALID_ARGUMENT &&
	      v92_phase4_modulator_next(&modulator, 0u, NULL) ==
		      V92_PHASE4_MODULATOR_INVALID_ARGUMENT &&
	      v92_phase4_modulator_next(&modulator, 4u, &sample) ==
		      V92_PHASE4_MODULATOR_INVALID_ARGUMENT &&
	      memcmp(&modulator, &before, sizeof(modulator)) == 0 &&
	      sample == 1234,
	      "invalid V.92 Phase 4 group changed state or output");
	modulator.scrambler.polynomial =
		(enum v34_scrambler_polynomial)2;
	before = modulator;
	CHECK(v92_phase4_modulator_next(&modulator, 0u, &sample) ==
		      V92_PHASE4_MODULATOR_INVALID_STATE &&
	      memcmp(&modulator, &before, sizeof(modulator)) == 0 &&
	      sample == 1234,
	      "invalid V.92 Phase 4 state was not transactional");
	memset(&demodulator, 0xa5, sizeof(demodulator));
	demodulator_before = demodulator;
	CHECK(v92_phase4_demodulator_init(
		      NULL, V92_CONSTELLATION_4_POINT, 0u, TEST_LU) ==
		      V92_PHASE4_MODULATOR_INVALID_ARGUMENT &&
	      v92_phase4_demodulator_init(
		      &demodulator, V92_CONSTELLATION_4_POINT, 2u,
		      TEST_LU) == V92_PHASE4_MODULATOR_INVALID_ARGUMENT &&
	      memcmp(
		      &demodulator, &demodulator_before,
		      sizeof(demodulator)) == 0 &&
	      v92_phase4_demodulator_init(
		      &demodulator, V92_CONSTELLATION_4_POINT, 0u,
		      TEST_LU) == V92_PHASE4_MODULATOR_OK,
	      "invalid V.92 Phase 4 demodulator initialization changed state");
	demodulator_before = demodulator;
	CHECK(!v92_phase4_demodulator_is_valid(NULL) &&
	      v92_phase4_demodulator_next(
		      NULL, 1.0, &group, &nearest) ==
		      V92_PHASE4_MODULATOR_INVALID_ARGUMENT &&
	      v92_phase4_demodulator_next(
		      &demodulator, 1.0, NULL, &nearest) ==
		      V92_PHASE4_MODULATOR_INVALID_ARGUMENT &&
	      v92_phase4_demodulator_next(
		      &demodulator, 1.0, &group, NULL) ==
		      V92_PHASE4_MODULATOR_INVALID_ARGUMENT &&
	      v92_phase4_demodulator_next(
		      &demodulator, NAN, &group, &nearest) ==
		      V92_PHASE4_MODULATOR_INVALID_ARGUMENT &&
	      v92_phase4_demodulator_next(
		      &demodulator, 1.0, (uint8_t *)(void *)&demodulator,
		      &nearest) == V92_PHASE4_MODULATOR_INVALID_ARGUMENT &&
	      v92_phase4_demodulator_next(
		      &demodulator, 1.0, (uint8_t *)(void *)&nearest,
		      &nearest) ==
		      V92_PHASE4_MODULATOR_INVALID_ARGUMENT &&
	      memcmp(
		      &demodulator, &demodulator_before,
		      sizeof(demodulator)) == 0 &&
	      group == 0xa5u && nearest == 4321,
	      "invalid V.92 Phase 4 demodulation changed state or output");
	demodulator.descrambler.polynomial =
		(enum v34_scrambler_polynomial)2;
	demodulator_before = demodulator;
	CHECK(v92_phase4_demodulator_next(
		      &demodulator, 1.0, &group, &nearest) ==
		      V92_PHASE4_MODULATOR_INVALID_STATE &&
	      memcmp(
		      &demodulator, &demodulator_before,
		      sizeof(demodulator)) == 0 &&
	      group == 0xa5u && nearest == 4321,
	      "invalid V.92 Phase 4 demodulator state was not transactional");
	for (size_t index = 0; index < ARRAY_LENGTH(names); ++index)
		CHECK(strcmp(
			      v92_phase4_modulator_result_name(
				      (enum v92_phase4_modulator_result)index),
			      names[index]) == 0,
		      "V.92 Phase 4 result name %zu differed", index);
	CHECK(strcmp(
		      v92_phase4_modulator_result_name(
			      (enum v92_phase4_modulator_result)-1),
		      "unknown") == 0,
	      "unknown V.92 Phase 4 result name differed");
	return true;
}

int main(void)
{
	fprintf(stderr, "[ RUN      ] V.92 Phase 4 modulator\n");
	if (!test_generated_levels() || !test_arbitrary_groups() ||
	    !test_contracts()) {
		fprintf(stderr, "[  FAILED  ] V.92 Phase 4 modulator\n");
		return 1;
	}
	fprintf(stderr, "[       OK ] V.92 Phase 4 modulator\n");
	return 0;
}
