// SPDX-License-Identifier: BSD-3-Clause

#include <v90_sign_encoder.h>

#include "test.h"

#include <stdint.h>
#include <string.h>

struct reference_state {
	uint8_t previous_t[V90_CP_FRAME_INTERVALS];
	uint8_t previous_odd;
	uint8_t previous_sign;
};

static void reference_sr0(
	struct reference_state *state, const uint8_t *bits,
	uint8_t signs[V90_CP_FRAME_INTERVALS])
{
	for (size_t index = 0;
	     index < V90_CP_FRAME_INTERVALS; ++index) {
		state->previous_sign ^= bits[index];
		signs[index] = state->previous_sign;
	}
}

static void reference_sr1(
	struct reference_state *state, const uint8_t *bits,
	uint8_t signs[V90_CP_FRAME_INTERVALS])
{
	uint8_t p[6];

	p[0] = 0;
	p[1] = bits[0] ^ state->previous_odd;
	p[2] = bits[1];
	p[3] = bits[2] ^ p[1];
	p[4] = bits[3];
	p[5] = bits[4] ^ p[3];
	for (size_t index = 0; index < ARRAY_LENGTH(p); ++index) {
		signs[index] = p[index] ^ state->previous_t[index];
		state->previous_t[index] = signs[index];
	}
	state->previous_odd = p[5];
}

static void reference_sr2(
	struct reference_state *state, const uint8_t *bits,
	uint8_t signs[V90_CP_FRAME_INTERVALS])
{
	uint8_t first_p[3] = {
		0, bits[0] ^ state->previous_odd, bits[1],
	};
	uint8_t second_p[3] = {
		0, bits[2] ^ first_p[1], bits[3],
	};
	uint8_t first_t[3];
	uint8_t second_t[3];

	for (size_t index = 0; index < ARRAY_LENGTH(first_p); ++index) {
		first_t[index] = first_p[index] ^ state->previous_t[index];
		second_t[index] = second_p[index] ^ first_t[index];
		signs[index] = first_t[index];
		signs[index + 3u] = second_t[index];
		state->previous_t[index] = second_t[index];
	}
	state->previous_odd = second_p[1];
}

static void reference_sr3(
	struct reference_state *state, const uint8_t *bits,
	uint8_t signs[V90_CP_FRAME_INTERVALS])
{
	uint8_t first_p[2] = {
		0, bits[0] ^ state->previous_odd,
	};
	uint8_t second_p[2] = {
		0, bits[1] ^ first_p[1],
	};
	uint8_t third_p[2] = {
		0, bits[2] ^ second_p[1],
	};
	uint8_t first_t[2];
	uint8_t second_t[2];
	uint8_t third_t[2];

	for (size_t index = 0; index < ARRAY_LENGTH(first_p); ++index) {
		first_t[index] = first_p[index] ^ state->previous_t[index];
		second_t[index] = second_p[index] ^ first_t[index];
		third_t[index] = third_p[index] ^ second_t[index];
		signs[index] = first_t[index];
		signs[index + 2u] = second_t[index];
		signs[index + 4u] = third_t[index];
		state->previous_t[index] = third_t[index];
	}
	state->previous_odd = third_p[1];
}

static void reference_encode(
	struct reference_state *state, uint8_t sign_redundancy,
	const uint8_t *bits,
	uint8_t signs[V90_CP_FRAME_INTERVALS])
{
	switch (sign_redundancy) {
	case 0:
		reference_sr0(state, bits, signs);
		break;
	case 1:
		reference_sr1(state, bits, signs);
		break;
	case 2:
		reference_sr2(state, bits, signs);
		break;
	default:
		reference_sr3(state, bits, signs);
		break;
	}
}

static bool state_matches(
	const struct v90_sign_encoder *encoder,
	const struct reference_state *reference,
	uint8_t sign_redundancy)
{
	size_t active = sign_redundancy == 0 ?
			0 : V90_CP_FRAME_INTERVALS / sign_redundancy;

	return encoder->sign_redundancy == sign_redundancy &&
	       encoder->previous_odd == reference->previous_odd &&
	       encoder->previous_pcm_sign == reference->previous_sign &&
	       memcmp(encoder->previous_shaping_signs,
		      reference->previous_t, active) == 0;
}

static bool test_all_redundancies(void)
{
	for (uint8_t redundancy = 0; redundancy <= 3; ++redundancy) {
		struct v90_sign_encoder encoder;
		struct reference_state reference = { 0 };
		size_t sign_bits = V90_CP_FRAME_INTERVALS - redundancy;

		CHECK(v90_sign_encoder_init(&encoder, redundancy) ==
			      V90_SIGN_ENCODER_OK &&
		      v90_sign_encoder_is_valid(&encoder),
		      "Sr=%u did not initialize", redundancy);
		for (unsigned int iteration = 0; iteration < 128; ++iteration) {
			uint8_t bits[V90_CP_FRAME_INTERVALS] = { 0 };
			uint8_t actual[V90_CP_FRAME_INTERVALS];
			uint8_t expected[V90_CP_FRAME_INTERVALS];
			unsigned int pattern =
				iteration * 13u + redundancy * 7u;

			for (size_t bit = 0; bit < sign_bits; ++bit)
				bits[bit] = (uint8_t)((pattern >> bit) & 1u);
			reference_encode(
				&reference, redundancy, bits, expected);
			CHECK(v90_sign_encoder_encode(
				      &encoder, bits, sign_bits, actual) ==
				      V90_SIGN_ENCODER_OK &&
			      memcmp(actual, expected, sizeof(actual)) == 0 &&
			      state_matches(
				      &encoder, &reference, redundancy) &&
			      v90_sign_encoder_is_valid(&encoder),
			      "Sr=%u changed at generated frame %u",
			      redundancy, iteration);
		}
	}
	return true;
}

static bool test_zero_initialization(void)
{
	for (uint8_t redundancy = 0; redundancy <= 3; ++redundancy) {
		struct v90_sign_encoder encoder;
		uint8_t bits[V90_CP_FRAME_INTERVALS] = { 0 };
		uint8_t signs[V90_CP_FRAME_INTERVALS];
		size_t sign_bits = V90_CP_FRAME_INTERVALS - redundancy;

		CHECK(v90_sign_encoder_init(&encoder, redundancy) ==
			      V90_SIGN_ENCODER_OK &&
		      v90_sign_encoder_encode(
			      &encoder, bits, sign_bits, signs) ==
			      V90_SIGN_ENCODER_OK,
		      "zero Sr=%u frame did not encode", redundancy);
		for (size_t index = 0; index < ARRAY_LENGTH(signs); ++index)
			CHECK(signs[index] == 0,
			      "zero Sr=%u frame changed sign %zu",
			      redundancy, index);
	}
	return true;
}

static bool test_contracts(void)
{
	struct v90_sign_encoder encoder;
	struct v90_sign_encoder saved;
	struct v90_sign_encoder untouched;
	struct v90_sign_encoder untouched_saved;
	uint8_t bits[V90_CP_FRAME_INTERVALS] = { 0 };
	uint8_t signs[V90_CP_FRAME_INTERVALS];
	uint8_t saved_signs[V90_CP_FRAME_INTERVALS];

	memset(&untouched, 0xa5, sizeof(untouched));
	untouched_saved = untouched;
	CHECK(v90_sign_encoder_init(NULL, 0) ==
		      V90_SIGN_ENCODER_INVALID_ARGUMENT &&
	      v90_sign_encoder_init(&untouched, 4) ==
		      V90_SIGN_ENCODER_INVALID_ARGUMENT &&
	      memcmp(&untouched, &untouched_saved, sizeof(untouched)) == 0,
	      "rejected initialization changed its destination");
	CHECK(v90_sign_encoder_init(&encoder, 2) ==
		      V90_SIGN_ENCODER_OK,
	      "contract fixture did not initialize");
	memset(signs, 0x5a, sizeof(signs));
	memcpy(saved_signs, signs, sizeof(signs));
	saved = encoder;
	bits[2] = 2;
	CHECK(v90_sign_encoder_encode(
		      &encoder, bits, 4, signs) ==
		      V90_SIGN_ENCODER_INVALID_BITS &&
	      memcmp(&encoder, &saved, sizeof(encoder)) == 0 &&
	      memcmp(signs, saved_signs, sizeof(signs)) == 0,
	      "non-binary input changed the encoder");
	bits[2] = 0;
	CHECK(v90_sign_encoder_encode(
		      &encoder, bits, 3, signs) ==
		      V90_SIGN_ENCODER_INVALID_BITS &&
	      v90_sign_encoder_encode(NULL, bits, 4, signs) ==
		      V90_SIGN_ENCODER_INVALID_ARGUMENT &&
	      v90_sign_encoder_encode(&encoder, NULL, 4, signs) ==
		      V90_SIGN_ENCODER_INVALID_ARGUMENT &&
	      v90_sign_encoder_encode(&encoder, bits, 4, NULL) ==
		      V90_SIGN_ENCODER_INVALID_ARGUMENT &&
	      memcmp(&encoder, &saved, sizeof(encoder)) == 0 &&
	      memcmp(signs, saved_signs, sizeof(signs)) == 0,
	      "rejected call was not transactional");

	encoder.previous_shaping_signs[3] = 1;
	CHECK(!v90_sign_encoder_is_valid(&encoder) &&
	      v90_sign_encoder_encode(&encoder, bits, 4, signs) ==
		      V90_SIGN_ENCODER_INVALID_STATE,
	      "encoder accepted inactive shaping state");
	encoder = saved;
	encoder.previous_odd = 2;
	CHECK(!v90_sign_encoder_is_valid(&encoder) &&
	      !v90_sign_encoder_is_valid(NULL) &&
	      strcmp(v90_sign_encoder_result_name(
		     V90_SIGN_ENCODER_INVALID_BITS),
		     "invalid bits") == 0 &&
	      strcmp(v90_sign_encoder_result_name(
		     (enum v90_sign_encoder_result)99),
		     "unknown") == 0,
	      "state validation or result names changed");
	return true;
}

struct test_case {
	const char *name;
	bool (*run)(void);
};

int main(void)
{
	static const struct test_case tests[] = {
		{ "all redundancies", test_all_redundancies },
		{ "zero initialization", test_zero_initialization },
		{ "contracts", test_contracts },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		printf("[ RUN      ] %s\n", tests[index].name);
		if (tests[index].run())
			printf("[       OK ] %s\n", tests[index].name);
		else {
			printf("[  FAILED  ] %s\n", tests[index].name);
			failures++;
		}
	}
	return failures == 0 ? 0 : 1;
}
