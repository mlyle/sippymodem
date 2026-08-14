// SPDX-License-Identifier: BSD-3-Clause

#include <v92_pcm_phase4_trn2u.h>

#include "test.h"

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
	TEST_LU = 5000,
	SEQUENCE_SYMBOLS = 96,
	MAX_BITS_PER_SYMBOL = 3,
};

static const int16_t FOUR_POINT_MAGNITUDES[] = { 2236, 6708 };
static const int16_t EIGHT_POINT_MAGNITUDES[] = {
	1091, 3273, 5455, 7638,
};

static const int16_t *magnitudes(
	enum v92_constellation_size constellation, size_t *count)
{
	if (constellation == V92_CONSTELLATION_4_POINT) {
		*count = ARRAY_LENGTH(FOUR_POINT_MAGNITUDES);
		return FOUR_POINT_MAGNITUDES;
	}
	*count = ARRAY_LENGTH(EIGHT_POINT_MAGNITUDES);
	return EIGHT_POINT_MAGNITUDES;
}

static bool test_normalized_levels(void)
{
	static const enum v92_constellation_size constellations[] = {
		V92_CONSTELLATION_4_POINT,
		V92_CONSTELLATION_8_POINT,
	};

	for (size_t selection = 0;
	     selection < ARRAY_LENGTH(constellations); ++selection) {
		size_t count;
		const int16_t *expected = magnitudes(
			constellations[selection], &count);

		CHECK(v92_pcm_phase4_trn2u_bits_per_symbol(
			      constellations[selection]) == selection + 2u,
		      "TRN2u bit width differed");
		for (size_t magnitude = 0; magnitude < count; ++magnitude) {
			for (uint8_t sign = 0; sign <= 1u; ++sign) {
				int16_t sample = 0;

				CHECK(v92_pcm_phase4_trn2u_level(
					      constellations[selection], TEST_LU,
					      (uint8_t)magnitude, sign, &sample) ==
					      V92_PCM_PHASE4_TRN2U_OK &&
				      sample == (sign != 0u ?
						 -expected[magnitude] :
						 expected[magnitude]),
				      "TRN2u level differed for %u points, %zu/%u",
				      (unsigned int)constellations[selection],
				      magnitude, sign);
			}
		}
	}
	return true;
}

static uint8_t expected_scrambled_bit(
	const uint8_t *bits, size_t index)
{
	uint8_t bit = 1u;

	if (index >= 5u)
		bit ^= bits[index - 5u];
	if (index >= 23u)
		bit ^= bits[index - 23u];
	return bit;
}

static bool check_sequence(
	enum v92_constellation_size constellation,
	uint8_t initial_previous_sign)
{
	uint8_t bits[SEQUENCE_SYMBOLS * MAX_BITS_PER_SYMBOL] = { 0 };
	struct v34_scrambler descrambler;
	struct v92_pcm_phase4_trn2u trn2u;
	uint32_t expected_history = 0;
	uint8_t previous_sign = initial_previous_sign;
	unsigned int bit_count =
		v92_pcm_phase4_trn2u_bits_per_symbol(constellation);
	size_t magnitude_count;
	const int16_t *levels = magnitudes(constellation, &magnitude_count);

	CHECK(v92_pcm_phase4_trn2u_init(
		      &trn2u, constellation, initial_previous_sign,
		      TEST_LU) == V92_PCM_PHASE4_TRN2U_OK &&
	      v34_scrambler_init(&descrambler, V34_POLYNOMIAL_GPA) ==
		      V34_FRAMING_OK &&
	      trn2u.scrambler.history == 0u &&
	      trn2u.scrambler.polynomial == V34_POLYNOMIAL_GPA &&
	      trn2u.previous_encoded_sign == initial_previous_sign &&
	      trn2u.emitted_symbols == 0u,
	      "TRN2u sequence did not initialize");
	for (size_t symbol_index = 0;
	     symbol_index < SEQUENCE_SYMBOLS; ++symbol_index) {
		uint8_t group = 0;

		for (unsigned int group_bit = 0;
		     group_bit < bit_count; ++group_bit) {
			size_t bit_index = symbol_index * bit_count + group_bit;
			uint8_t decoded = 0;

			bits[bit_index] = expected_scrambled_bit(bits, bit_index);
			group |= (uint8_t)(bits[bit_index] << group_bit);
			expected_history =
				((expected_history << 1) | bits[bit_index]) &
				UINT32_C(0x7fffff);
			CHECK(v34_descramble_group(
				      &descrambler, bits[bit_index], 1u,
				      &decoded) == V34_FRAMING_OK &&
			      decoded == 1u,
			      "TRN2u bit %zu did not descramble to one",
			      bit_index);
		}
		uint8_t sign_input = (uint8_t)(group >> (bit_count - 1u));
		uint8_t encoded_sign = (uint8_t)(sign_input ^ previous_sign);
		uint8_t magnitude = (uint8_t)(
			group & ((1u << (bit_count - 1u)) - 1u));
		int16_t sample = 0;
		int16_t expected = levels[magnitude];

		if (encoded_sign != 0u)
			expected = (int16_t)-expected;
		CHECK(magnitude < magnitude_count &&
		      v92_pcm_phase4_trn2u_next(&trn2u, &sample) ==
			      V92_PCM_PHASE4_TRN2U_OK &&
		      sample == expected &&
		      trn2u.previous_encoded_sign == encoded_sign &&
		      trn2u.scrambler.history == expected_history &&
		      trn2u.emitted_symbols == symbol_index + 1u &&
		      v92_pcm_phase4_trn2u_is_valid(&trn2u),
		      "TRN2u sequence differed at symbol %zu", symbol_index);
		previous_sign = encoded_sign;
	}
	return true;
}

static bool test_sequences(void)
{
	for (enum v92_constellation_size constellation =
		     V92_CONSTELLATION_4_POINT;
	     constellation <= V92_CONSTELLATION_8_POINT;
	     constellation = V92_CONSTELLATION_8_POINT) {
		for (uint8_t previous_sign = 0;
		     previous_sign <= 1u; ++previous_sign)
			CHECK(check_sequence(constellation, previous_sign),
			      "TRN2u generated sequence failed");
		if (constellation == V92_CONSTELLATION_8_POINT)
			break;
	}
	return true;
}

static bool test_initial_boundary(void)
{
	static const enum v92_constellation_size constellations[] = {
		V92_CONSTELLATION_4_POINT,
		V92_CONSTELLATION_8_POINT,
	};

	for (size_t selection = 0;
	     selection < ARRAY_LENGTH(constellations); ++selection) {
		struct v92_pcm_phase4_trn2u trn2u;

		CHECK(v92_pcm_phase4_trn2u_init(
			      &trn2u, constellations[selection], 1u,
			      TEST_LU) == V92_PCM_PHASE4_TRN2U_OK,
		      "TRN2u boundary did not initialize");
		for (uint32_t count = 1;
		     count <= V92_PCM_PHASE4_TRN2U_INITIAL_MIN_SYMBOLS +
			      V92_PCM_PHASE4_TRN2U_ALIGNMENT;
		     ++count) {
			int16_t sample;
			bool expected =
				count >= V92_PCM_PHASE4_TRN2U_INITIAL_MIN_SYMBOLS &&
				count % V92_PCM_PHASE4_TRN2U_ALIGNMENT == 0u;

			CHECK(v92_pcm_phase4_trn2u_next(&trn2u, &sample) ==
				      V92_PCM_PHASE4_TRN2U_OK &&
			      v92_pcm_phase4_trn2u_can_handoff(&trn2u) ==
				      expected,
			      "TRN2u handoff boundary differed at %u", count);
		}
	}
	return true;
}

static bool test_contracts(void)
{
	struct v92_pcm_phase4_trn2u trn2u;
	struct v92_pcm_phase4_trn2u before;
	int16_t sample = 1234;
	static const char *const names[] = {
		"ok", "invalid argument", "invalid state",
	};

	memset(&trn2u, 0xa5, sizeof(trn2u));
	before = trn2u;
	CHECK(v92_pcm_phase4_trn2u_bits_per_symbol(
		      (enum v92_constellation_size)6) == 0u &&
	      v92_pcm_phase4_trn2u_init(NULL, V92_CONSTELLATION_4_POINT,
		      0u, TEST_LU) == V92_PCM_PHASE4_TRN2U_INVALID_ARGUMENT &&
	      v92_pcm_phase4_trn2u_init(
		      &trn2u, (enum v92_constellation_size)6, 0u,
		      TEST_LU) == V92_PCM_PHASE4_TRN2U_INVALID_ARGUMENT &&
	      v92_pcm_phase4_trn2u_init(
		      &trn2u, V92_CONSTELLATION_4_POINT, 2u,
		      TEST_LU) == V92_PCM_PHASE4_TRN2U_INVALID_ARGUMENT &&
	      v92_pcm_phase4_trn2u_init(
		      &trn2u, V92_CONSTELLATION_8_POINT, 0u,
		      INT16_MAX) == V92_PCM_PHASE4_TRN2U_INVALID_ARGUMENT &&
	      memcmp(&trn2u, &before, sizeof(trn2u)) == 0,
	      "invalid TRN2u initialization changed its destination");
	CHECK(v92_pcm_phase4_trn2u_level(
		      V92_CONSTELLATION_4_POINT, TEST_LU, 2u, 0u,
		      &sample) == V92_PCM_PHASE4_TRN2U_INVALID_ARGUMENT &&
	      v92_pcm_phase4_trn2u_level(
		      V92_CONSTELLATION_8_POINT, TEST_LU, 0u, 2u,
		      &sample) == V92_PCM_PHASE4_TRN2U_INVALID_ARGUMENT &&
	      v92_pcm_phase4_trn2u_level(
		      V92_CONSTELLATION_8_POINT, INT16_MAX, 0u, 0u,
		      &sample) == V92_PCM_PHASE4_TRN2U_INVALID_ARGUMENT &&
	      v92_pcm_phase4_trn2u_level(
		      V92_CONSTELLATION_4_POINT, TEST_LU, 0u, 0u,
		      NULL) == V92_PCM_PHASE4_TRN2U_INVALID_ARGUMENT &&
	      sample == 1234,
	      "invalid TRN2u level request changed its output");
	CHECK(v92_pcm_phase4_trn2u_init(
		      &trn2u, V92_CONSTELLATION_4_POINT, 0u,
		      TEST_LU) == V92_PCM_PHASE4_TRN2U_OK,
	      "TRN2u contract state did not initialize");
	trn2u.emitted_symbols = UINT32_MAX;
	before = trn2u;
	CHECK(!v92_pcm_phase4_trn2u_is_valid(NULL) &&
	      !v92_pcm_phase4_trn2u_can_handoff(NULL) &&
	      v92_pcm_phase4_trn2u_next(NULL, &sample) ==
		      V92_PCM_PHASE4_TRN2U_INVALID_ARGUMENT &&
	      v92_pcm_phase4_trn2u_next(&trn2u, NULL) ==
		      V92_PCM_PHASE4_TRN2U_INVALID_ARGUMENT &&
	      v92_pcm_phase4_trn2u_next(&trn2u, &sample) ==
		      V92_PCM_PHASE4_TRN2U_INVALID_STATE &&
	      memcmp(&trn2u, &before, sizeof(trn2u)) == 0 && sample == 1234,
	      "invalid TRN2u generation was not transactional");
	for (size_t index = 0; index < ARRAY_LENGTH(names); ++index)
		CHECK(strcmp(
			      v92_pcm_phase4_trn2u_result_name(
				      (enum v92_pcm_phase4_trn2u_result)index),
			      names[index]) == 0,
		      "TRN2u result name %zu differed", index);
	CHECK(strcmp(
		      v92_pcm_phase4_trn2u_result_name(
			      (enum v92_pcm_phase4_trn2u_result)-1),
		      "unknown") == 0,
	      "unknown TRN2u result name differed");
	return true;
}

int main(void)
{
	fprintf(stderr, "[ RUN      ] V.92 initial TRN2u\n");
	if (!test_normalized_levels() || !test_sequences() ||
	    !test_initial_boundary() || !test_contracts()) {
		fprintf(stderr, "[  FAILED  ] V.92 initial TRN2u\n");
		return 1;
	}
	fprintf(stderr, "[       OK ] V.92 initial TRN2u\n");
	return 0;
}
