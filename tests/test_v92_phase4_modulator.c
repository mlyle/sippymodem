// SPDX-License-Identifier: BSD-3-Clause

#include <v92_phase4_modulator.h>

#include "test.h"

#include <limits.h>
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

static bool sample_components(
	enum v92_constellation_size constellation, int16_t sample,
	uint8_t *magnitude_index, uint8_t *encoded_sign)
{
	size_t count = 0;
	const int16_t *expected = magnitudes(constellation, &count);
	int32_t magnitude = sample < 0 ? -(int32_t)sample : sample;

	if (expected == NULL || sample == 0 || magnitude_index == NULL ||
	    encoded_sign == NULL)
		return false;
	for (size_t index = 0; index < count; ++index) {
		if (magnitude != expected[index])
			continue;
		*magnitude_index = (uint8_t)index;
		*encoded_sign = sample < 0 ? 1u : 0u;
		return true;
	}
	return false;
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
	struct v34_scrambler descrambler;
	unsigned int bits =
		v92_phase4_modulator_bits_per_symbol(constellation);
	uint8_t group_count = (uint8_t)(1u << bits);
	uint8_t previous_sign = initial_previous_sign;

	CHECK(v92_phase4_modulator_init(
		      &modulator, constellation, initial_previous_sign,
		      TEST_LU) == V92_PHASE4_MODULATOR_OK &&
	      v34_scrambler_init(&descrambler, V34_POLYNOMIAL_GPA) ==
		      V34_FRAMING_OK &&
	      v92_phase4_modulator_is_valid(&modulator),
	      "V.92 Phase 4 group state did not initialize");
	for (unsigned int cycle = 0; cycle < GROUP_CYCLES; ++cycle) {
		for (uint8_t input_group = 0;
		     input_group < group_count; ++input_group) {
			uint8_t magnitude_index;
			uint8_t encoded_sign;
			uint8_t scrambled_group;
			uint8_t recovered_group = 0;
			int16_t sample = 0;

			CHECK(v92_phase4_modulator_next(
				      &modulator, input_group, &sample) ==
				      V92_PHASE4_MODULATOR_OK &&
			      sample_components(
				      constellation, sample, &magnitude_index,
				      &encoded_sign),
			      "V.92 Phase 4 group %u/%u did not modulate",
			      cycle, input_group);
			scrambled_group = (uint8_t)(
				magnitude_index |
				((encoded_sign ^ previous_sign) << (bits - 1u)));
			for (unsigned int bit = 0; bit < bits; ++bit) {
				uint8_t recovered_bit = 0;

				CHECK(v34_descramble_group(
					      &descrambler,
					      (uint8_t)((scrambled_group >> bit) & 1u),
					      1u, &recovered_bit) == V34_FRAMING_OK,
				      "V.92 Phase 4 group bit did not descramble");
				recovered_group |= (uint8_t)(recovered_bit << bit);
			}
			CHECK(recovered_group == input_group &&
			      modulator.previous_encoded_sign == encoded_sign &&
			      modulator.scrambler.history == descrambler.history &&
			      v92_phase4_modulator_is_valid(&modulator),
			      "V.92 Phase 4 continuity differed at %u/%u",
			      cycle, input_group);
			previous_sign = encoded_sign;
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
