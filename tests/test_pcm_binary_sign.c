// SPDX-License-Identifier: BSD-3-Clause

#include <pcm_binary_sign.h>

#include "test.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
	STREAM_BITS = 512,
	TRAINING_BITS = 257,
};

static uint8_t generated_bit(size_t index, unsigned int salt)
{
	uint32_t value = (uint32_t)index * UINT32_C(0x9e3779b1) + salt;

	value ^= value >> 11u;
	value ^= value << 7u;
	return (uint8_t)((value >> (index % 17u)) & 1u);
}

static bool states_match(
	const struct pcm_binary_sign *first,
	const struct pcm_binary_sign *second)
{
	return first->scrambler.polynomial == second->scrambler.polynomial &&
	       first->scrambler.history == second->scrambler.history &&
	       first->previous_sign == second->previous_sign;
}

static bool test_generated_round_trip(void)
{
	for (enum v34_scrambler_polynomial polynomial = V34_POLYNOMIAL_GPC;
	     polynomial <= V34_POLYNOMIAL_GPA; ++polynomial) {
		struct pcm_binary_sign encoder;
		struct pcm_binary_sign decoder;

		CHECK(pcm_binary_sign_init(&encoder, polynomial) ==
			      PCM_BINARY_SIGN_OK &&
		      pcm_binary_sign_init(&decoder, polynomial) ==
			      PCM_BINARY_SIGN_OK,
		      "binary-sign pair %u did not initialize",
		      (unsigned int)polynomial);
		for (size_t index = 0; index < STREAM_BITS; ++index) {
			uint8_t input = generated_bit(
				index, (unsigned int)polynomial * 29u);
			uint8_t sign = UINT8_C(0xa5);
			uint8_t output = UINT8_C(0x5a);

			CHECK(pcm_binary_sign_encode(
				      &encoder, input, &sign) ==
				      PCM_BINARY_SIGN_OK &&
			      pcm_binary_sign_decode(
				      &decoder, sign, &output) ==
				      PCM_BINARY_SIGN_OK &&
			      output == input && states_match(&encoder, &decoder),
			      "binary-sign pair %u differed at bit %zu",
			      (unsigned int)polynomial, index);
		}
	}
	return true;
}

static bool test_training_handoff(void)
{
	struct v34_scrambler source;
	struct pcm_binary_sign encoder;
	struct pcm_binary_sign decoder;
	uint8_t final_sign = 0u;

	CHECK(v34_scrambler_init(&source, V34_POLYNOMIAL_GPA) ==
		      V34_FRAMING_OK &&
	      pcm_binary_sign_init(&decoder, V34_POLYNOMIAL_GPA) ==
		      PCM_BINARY_SIGN_OK,
	      "binary-sign training fixture did not initialize");
	for (size_t index = 0; index < TRAINING_BITS; ++index) {
		CHECK(v34_scramble_group(
			      &source, 1u, 1u, &final_sign) ==
			      V34_FRAMING_OK &&
		      pcm_binary_sign_track_scrambler_output(
			      &decoder, final_sign) == PCM_BINARY_SIGN_OK,
		      "binary-sign training stopped at bit %zu", index);
	}
	CHECK(decoder.scrambler.history == source.history &&
	      decoder.previous_sign == final_sign &&
	      pcm_binary_sign_init_from_scrambler(
		      &encoder, &source, final_sign) == PCM_BINARY_SIGN_OK &&
	      states_match(&encoder, &decoder),
	      "binary-sign training did not preserve its handoff state");
	for (size_t index = 0; index < STREAM_BITS; ++index) {
		uint8_t input = generated_bit(index, 71u);
		uint8_t sign;
		uint8_t output;

		CHECK(pcm_binary_sign_encode(&encoder, input, &sign) ==
			      PCM_BINARY_SIGN_OK &&
		      pcm_binary_sign_decode(&decoder, sign, &output) ==
			      PCM_BINARY_SIGN_OK &&
		      output == input && states_match(&encoder, &decoder),
		      "binary-sign handoff differed at bit %zu", index);
	}
	return true;
}

static bool test_contracts(void)
{
	struct pcm_binary_sign state;
	struct pcm_binary_sign before;
	struct v34_scrambler scrambler;
	uint8_t output = UINT8_C(0xa5);
	static const char *const names[] = {
		"ok", "invalid argument", "invalid state",
	};

	memset(&state, 0xa5, sizeof(state));
	before = state;
	CHECK(pcm_binary_sign_init(NULL, V34_POLYNOMIAL_GPA) ==
		      PCM_BINARY_SIGN_INVALID_ARGUMENT &&
	      pcm_binary_sign_init(
		      &state, (enum v34_scrambler_polynomial)-1) ==
		      PCM_BINARY_SIGN_INVALID_ARGUMENT &&
	      pcm_binary_sign_init_from_scrambler(
		      &state, NULL, 0u) == PCM_BINARY_SIGN_INVALID_ARGUMENT &&
	      memcmp(&state, &before, sizeof(state)) == 0,
	      "invalid binary-sign initialization changed its destination");
	CHECK(v34_scrambler_init(&scrambler, V34_POLYNOMIAL_GPA) ==
		      V34_FRAMING_OK &&
	      pcm_binary_sign_init_from_scrambler(
		      &state, &scrambler, 2u) ==
		      PCM_BINARY_SIGN_INVALID_ARGUMENT &&
	      memcmp(&state, &before, sizeof(state)) == 0 &&
	      pcm_binary_sign_init(&state, V34_POLYNOMIAL_GPA) ==
		      PCM_BINARY_SIGN_OK,
	      "binary-sign contract fixture did not initialize");
	before = state;
	CHECK(pcm_binary_sign_track_scrambler_output(NULL, 0u) ==
		      PCM_BINARY_SIGN_INVALID_ARGUMENT &&
	      pcm_binary_sign_track_scrambler_output(&state, 2u) ==
		      PCM_BINARY_SIGN_INVALID_ARGUMENT &&
	      pcm_binary_sign_encode(NULL, 0u, &output) ==
		      PCM_BINARY_SIGN_INVALID_ARGUMENT &&
	      pcm_binary_sign_encode(&state, 2u, &output) ==
		      PCM_BINARY_SIGN_INVALID_ARGUMENT &&
	      pcm_binary_sign_encode(&state, 0u, NULL) ==
		      PCM_BINARY_SIGN_INVALID_ARGUMENT &&
	      pcm_binary_sign_decode(NULL, 0u, &output) ==
		      PCM_BINARY_SIGN_INVALID_ARGUMENT &&
	      pcm_binary_sign_decode(&state, 2u, &output) ==
		      PCM_BINARY_SIGN_INVALID_ARGUMENT &&
	      pcm_binary_sign_decode(&state, 0u, NULL) ==
		      PCM_BINARY_SIGN_INVALID_ARGUMENT &&
	      memcmp(&state, &before, sizeof(state)) == 0 &&
	      output == UINT8_C(0xa5),
	      "invalid binary-sign call changed state or output");
	state.previous_sign = 2u;
	before = state;
	CHECK(!pcm_binary_sign_is_valid(&state) &&
	      !pcm_binary_sign_is_valid(NULL) &&
	      pcm_binary_sign_track_scrambler_output(&state, 0u) ==
		      PCM_BINARY_SIGN_INVALID_STATE &&
	      pcm_binary_sign_encode(&state, 0u, &output) ==
		      PCM_BINARY_SIGN_INVALID_STATE &&
	      pcm_binary_sign_decode(&state, 0u, &output) ==
		      PCM_BINARY_SIGN_INVALID_STATE &&
	      memcmp(&state, &before, sizeof(state)) == 0 &&
	      output == UINT8_C(0xa5),
	      "corrupt binary-sign state was not rejected transactionally");
	for (size_t index = 0; index < ARRAY_LENGTH(names); ++index)
		CHECK(strcmp(
			      pcm_binary_sign_result_name(
				      (enum pcm_binary_sign_result)index),
			      names[index]) == 0,
		      "binary-sign result name %zu differed", index);
	CHECK(strcmp(
		      pcm_binary_sign_result_name(
			      (enum pcm_binary_sign_result)-1),
		      "unknown") == 0,
	      "unknown binary-sign result name differed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "generated round trip", test_generated_round_trip },
		{ "training handoff", test_training_handoff },
		{ "contracts", test_contracts },
	};
	unsigned int failures = 0u;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		if (tests[index].run())
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		else {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failures++;
		}
	}
	return failures == 0u ? 0 : 1;
}
