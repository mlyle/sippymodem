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
		v92_phase4_modulator_bits_per_symbol(constellation);
	size_t magnitude_count;
	const int16_t *levels = magnitudes(constellation, &magnitude_count);

	CHECK(v92_pcm_phase4_trn2u_init(
		      &trn2u, constellation, initial_previous_sign,
		      TEST_LU) == V92_PCM_PHASE4_TRN2U_OK &&
	      v34_scrambler_init(&descrambler, V34_POLYNOMIAL_GPA) ==
		      V34_FRAMING_OK &&
	      trn2u.modulator.scrambler.history == 0u &&
	      trn2u.modulator.scrambler.polynomial == V34_POLYNOMIAL_GPA &&
	      trn2u.modulator.previous_encoded_sign ==
		      initial_previous_sign &&
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
		      trn2u.modulator.previous_encoded_sign == encoded_sign &&
		      trn2u.modulator.scrambler.history == expected_history &&
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

static bool test_output_ownership(void)
{
	for (unsigned mode = 0; mode < 2u; ++mode) {
		for (uint8_t sign = 0; sign <= 1u; ++sign) {
			struct {
				int16_t before;
				struct v92_pcm_phase4_trn2u owner;
				int16_t after;
			} wrapped = { .before = 1234, .after = 5678 };
			struct v92_pcm_phase4_trn2u saved, expected;
			int16_t sample;

			CHECK(v92_pcm_phase4_trn2u_init(&wrapped.owner, mode ?
				      V92_CONSTELLATION_8_POINT : V92_CONSTELLATION_4_POINT,
				      sign, TEST_LU) == V92_PCM_PHASE4_TRN2U_OK,
			      "TRN2u output fixture did not initialize");
			for (unsigned symbol = 0; symbol < 8u; ++symbol)
				CHECK(v92_pcm_phase4_trn2u_next(&wrapped.owner, &sample) ==
					      V92_PCM_PHASE4_TRN2U_OK, "TRN2u warm-up failed");
			saved = wrapped.owner;
			CHECK(v92_pcm_phase4_trn2u_next(&wrapped.owner,
				      &wrapped.owner.modulator.lu) == V92_PCM_PHASE4_TRN2U_INVALID_ARGUMENT &&
			      memcmp(&wrapped.owner, &saved, sizeof(saved)) == 0,
			      "TRN2u overlapping sample changed coding state");
			/* The counter is outside the nested mapper; reject without dereferencing. */
			CHECK(v92_pcm_phase4_trn2u_next(&wrapped.owner,
				      (int16_t *)(void *)&wrapped.owner.emitted_symbols) ==
				      V92_PCM_PHASE4_TRN2U_INVALID_ARGUMENT &&
			      memcmp(&wrapped.owner, &saved, sizeof(saved)) == 0,
			      "TRN2u output overwrote its counter");
			expected = saved;
			CHECK(v92_pcm_phase4_trn2u_next(&expected, &sample) == V92_PCM_PHASE4_TRN2U_OK &&
			      v92_pcm_phase4_trn2u_next(&wrapped.owner, &wrapped.before) ==
				      V92_PCM_PHASE4_TRN2U_OK && wrapped.before == sample &&
			      wrapped.after == 5678 && memcmp(&wrapped.owner, &expected, sizeof(expected)) == 0,
			      "TRN2u preceding disjoint output differed");
			CHECK(v92_pcm_phase4_trn2u_next(&expected, &sample) == V92_PCM_PHASE4_TRN2U_OK &&
			      v92_pcm_phase4_trn2u_next(&wrapped.owner, &wrapped.after) ==
				      V92_PCM_PHASE4_TRN2U_OK && wrapped.after == sample &&
			      memcmp(&wrapped.owner, &expected, sizeof(expected)) == 0,
			      "TRN2u following disjoint output differed");
		}
	}
	return true;
}

static bool test_mapper_failure(void)
{
	struct v92_pcm_phase4_trn2u trn2u, saved;
	int16_t sample = 1234;

	/* LU=1 admits the outer level, but the fourth scrambled symbol rounds
	 * its inner level to zero after advancing the mapper's scratch history. */
	CHECK(v92_pcm_phase4_trn2u_init(&trn2u, V92_CONSTELLATION_4_POINT, 0u, 1) ==
		      V92_PCM_PHASE4_TRN2U_OK, "low-level TRN2u did not initialize");
	for (unsigned symbol = 0; symbol < 3u; ++symbol)
		CHECK(v92_pcm_phase4_trn2u_next(&trn2u, &sample) == V92_PCM_PHASE4_TRN2U_OK,
		      "low-level TRN2u failed before its inner symbol");
	saved = trn2u;
	sample = 1234;
	CHECK(v92_pcm_phase4_trn2u_is_valid(&trn2u) &&
	      v92_pcm_phase4_trn2u_next(&trn2u, &sample) == V92_PCM_PHASE4_TRN2U_INVALID_STATE &&
	      memcmp(&trn2u, &saved, sizeof(saved)) == 0 && sample == 1234,
	      "TRN2u mapper failure advanced history, counter or output");
	return true;
}

static bool test_contracts(void)
{
	struct v92_pcm_phase4_trn2u trn2u;
	struct v92_pcm_phase4_trn2u before;
	int16_t sample = 1234;

	memset(&trn2u, 0xa5, sizeof(trn2u));
	before = trn2u;
	CHECK(v92_pcm_phase4_trn2u_init(NULL, V92_CONSTELLATION_4_POINT,
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
	CHECK(v92_pcm_phase4_trn2u_init(
		      &trn2u, V92_CONSTELLATION_4_POINT, 0u,
		      TEST_LU) == V92_PCM_PHASE4_TRN2U_OK,
	      "TRN2u contract state did not initialize");
	trn2u.emitted_symbols = UINT32_MAX - 1u;
	CHECK(v92_pcm_phase4_trn2u_next(&trn2u, &sample) == V92_PCM_PHASE4_TRN2U_OK &&
	      trn2u.emitted_symbols == UINT32_MAX,
	      "TRN2u rejected its final representable counter increment");
	sample = 1234;
	before = trn2u;
	CHECK(!v92_pcm_phase4_trn2u_is_valid(NULL) &&
	      v92_pcm_phase4_trn2u_next(NULL, &sample) ==
		      V92_PCM_PHASE4_TRN2U_INVALID_ARGUMENT &&
	      v92_pcm_phase4_trn2u_next(&trn2u, NULL) ==
		      V92_PCM_PHASE4_TRN2U_INVALID_ARGUMENT &&
	      v92_pcm_phase4_trn2u_next(&trn2u, &trn2u.modulator.lu) ==
		      V92_PCM_PHASE4_TRN2U_INVALID_ARGUMENT &&
	      v92_pcm_phase4_trn2u_next(&trn2u, &sample) ==
		      V92_PCM_PHASE4_TRN2U_INVALID_STATE &&
	      memcmp(&trn2u, &before, sizeof(trn2u)) == 0 && sample == 1234,
	      "invalid TRN2u generation was not transactional");
	return true;
}

int main(void)
{
	fprintf(stderr, "[ RUN      ] V.92 initial TRN2u\n");
	if (!test_sequences() || !test_output_ownership() ||
	    !test_mapper_failure() || !test_contracts()) {
		fprintf(stderr, "[  FAILED  ] V.92 initial TRN2u\n");
		return 1;
	}
	fprintf(stderr, "[       OK ] V.92 initial TRN2u\n");
	return 0;
}
