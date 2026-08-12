// SPDX-License-Identifier: BSD-3-Clause

#include "test.h"

#include "g711.h"
#include "v90_phase3_signs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct reference_scrambler {
	uint8_t history[23];
};

static uint8_t reference_scramble(
	struct reference_scrambler *scrambler, uint8_t input)
{
	uint8_t output = input ^ scrambler->history[17] ^
			 scrambler->history[22];

	for (size_t delay = 22; delay > 0; --delay)
		scrambler->history[delay] = scrambler->history[delay - 1];
	scrambler->history[0] = output;
	return output;
}

static uint8_t encode_sample(enum g711_law law, int16_t sample)
{
	return law == G711_LAW_PCMA ? g711_encode_pcma(sample) :
					   g711_encode_pcmu(sample);
}

static bool check_symbol(
	const struct v90_pcm_symbol *symbol, enum g711_law law,
	uint8_t ucode, uint8_t sign)
{
	uint8_t positive = law == G711_LAW_PCMA ?
				(uint8_t)(ucode ^ 0xd5u) :
				(uint8_t)~ucode;
	uint8_t expected = sign != 0 ? positive :
					  (uint8_t)(positive ^ 0x80u);

	return symbol->ucode == ucode &&
	       symbol->positive == (sign != 0) &&
	       symbol->octet == expected &&
	       encode_sample(law, symbol->sample) == expected;
}

static bool run_continuous_stream(enum g711_law law)
{
	struct v90_phase3_signs signs;
	struct reference_scrambler reference = { 0 };
	uint8_t last_sign = 0;

	CHECK(v90_phase3_signs_init(&signs, law, 78) ==
		      V90_PHASE3_SIGNS_OK,
	      "sign stream initialization failed for law %u",
	      (unsigned int)law);
	for (size_t index = 0;
	     index < V90_PHASE3_TRN1D_MIN_SYMBOLS; ++index) {
		struct v90_pcm_symbol symbol;
		uint8_t sign = reference_scramble(&reference, 1);

		CHECK(v90_phase3_signs_next_trn1d(&signs, &symbol) ==
			      V90_PHASE3_SIGNS_OK &&
			      check_symbol(&symbol, law, 78, sign),
		      "TRN1d mismatch for law %u at symbol %zu",
		      (unsigned int)law, index);
		last_sign = sign;
	}
	CHECK(v90_phase3_signs_begin_jd(&signs) == V90_PHASE3_SIGNS_OK &&
		      signs.phase == V90_PHASE3_SIGNS_JD,
	      "TRN1d did not hand off to Jd at its minimum boundary");
	for (size_t index = 0; index < 72; ++index) {
		struct v90_pcm_symbol symbol;
		uint8_t bit = (uint8_t)(((index * 5u) ^ (index >> 1u)) & 1u);
		uint8_t scrambled = reference_scramble(&reference, bit);

		last_sign ^= scrambled;
		CHECK(v90_phase3_signs_next_jd_bit(&signs, bit, &symbol) ==
			      V90_PHASE3_SIGNS_OK &&
			      check_symbol(&symbol, law, 78, last_sign),
		      "Jd mismatch for law %u at bit %zu",
		      (unsigned int)law, index);
	}
	for (size_t index = 0; index < V90_PHASE3_JD_PRIME_BITS; ++index) {
		struct v90_pcm_symbol symbol;
		uint8_t scrambled = reference_scramble(&reference, 0);

		last_sign ^= scrambled;
		CHECK(v90_phase3_signs_next_jd_bit(&signs, 0, &symbol) ==
			      V90_PHASE3_SIGNS_OK &&
			      check_symbol(&symbol, law, 78, last_sign),
		      "Jd-prime mismatch for law %u at bit %zu",
		      (unsigned int)law, index);
	}
	return true;
}

static bool test_continuous_streams(void)
{
	return run_continuous_stream(G711_LAW_PCMU) &&
	       run_continuous_stream(G711_LAW_PCMA);
}

static bool test_jd_boundary(void)
{
	struct v90_phase3_signs signs;
	struct v90_phase3_signs saved;
	struct v90_pcm_symbol symbol;

	CHECK(v90_phase3_signs_init(&signs, G711_LAW_PCMU, 91) ==
		      V90_PHASE3_SIGNS_OK,
	      "boundary stream initialization failed");
	for (size_t index = 0;
	     index < V90_PHASE3_TRN1D_MIN_SYMBOLS - 1u; ++index)
		CHECK(v90_phase3_signs_next_trn1d(&signs, &symbol) ==
			      V90_PHASE3_SIGNS_OK,
		      "boundary TRN1d generation failed");
	saved = signs;
	CHECK(v90_phase3_signs_begin_jd(&signs) ==
		      V90_PHASE3_SIGNS_INVALID_STATE &&
		      memcmp(&signs, &saved, sizeof(signs)) == 0,
	      "early Jd handoff changed state");
	for (size_t index = 0; index < 2; ++index)
		CHECK(v90_phase3_signs_next_trn1d(&signs, &symbol) ==
			      V90_PHASE3_SIGNS_OK,
		      "unaligned boundary generation failed");
	saved = signs;
	CHECK(v90_phase3_signs_begin_jd(&signs) ==
		      V90_PHASE3_SIGNS_INVALID_STATE &&
		      memcmp(&signs, &saved, sizeof(signs)) == 0,
	      "unaligned Jd handoff changed state");
	for (size_t index = 0; index < 5; ++index)
		CHECK(v90_phase3_signs_next_trn1d(&signs, &symbol) ==
			      V90_PHASE3_SIGNS_OK,
		      "aligned boundary generation failed");
	CHECK(signs.trn1d_symbols == 2046 &&
		      v90_phase3_signs_begin_jd(&signs) == V90_PHASE3_SIGNS_OK,
	      "extended aligned TRN1d did not hand off");
	return true;
}

static bool test_validation(void)
{
	struct v90_phase3_signs signs;
	struct v90_phase3_signs saved;
	struct v90_pcm_symbol symbol = {
		.ucode = 0xa5,
		.octet = 0x5a,
		.sample = 1234,
	};
	struct v90_pcm_symbol saved_symbol = symbol;
	struct v90_pcm_symbol invalid_symbol = symbol;

	memset(&signs, 0xa5, sizeof(signs));
	saved = signs;
	CHECK(v90_phase3_signs_init(
		      &signs, (enum g711_law)2, 78) ==
		      V90_PHASE3_SIGNS_INVALID_ARGUMENT &&
		      v90_phase3_signs_init(&signs, G711_LAW_PCMU, 66) ==
			      V90_PHASE3_SIGNS_INVALID_ARGUMENT &&
		      v90_phase3_signs_init(NULL, G711_LAW_PCMU, 78) ==
			      V90_PHASE3_SIGNS_INVALID_ARGUMENT &&
		      memcmp(&signs, &saved, sizeof(signs)) == 0,
	      "invalid sign-stream initialization changed state");
	CHECK(v90_phase3_signs_next_trn1d(NULL, &symbol) ==
		      V90_PHASE3_SIGNS_INVALID_ARGUMENT &&
		      v90_phase3_signs_next_trn1d(&signs, NULL) ==
			      V90_PHASE3_SIGNS_INVALID_ARGUMENT &&
		      v90_phase3_signs_next_trn1d(&signs, &symbol) ==
			      V90_PHASE3_SIGNS_INVALID_STATE &&
		      v90_phase3_signs_begin_jd(NULL) ==
			      V90_PHASE3_SIGNS_INVALID_ARGUMENT &&
		      v90_phase3_signs_next_jd_bit(NULL, 0, &symbol) ==
			      V90_PHASE3_SIGNS_INVALID_ARGUMENT &&
		      v90_phase3_signs_next_jd_bit(&signs, 2, &symbol) ==
			      V90_PHASE3_SIGNS_INVALID_ARGUMENT &&
		      memcmp(&signs, &saved, sizeof(signs)) == 0 &&
		      memcmp(&symbol, &saved_symbol, sizeof(symbol)) == 0 &&
		      !v90_phase3_signs_is_valid(NULL),
	      "invalid sign-stream operation changed state or output");
	CHECK(!v90_pcm_symbol_make(
		      &invalid_symbol, (enum g711_law)2, 78, true) &&
		      !v90_pcm_symbol_make(
			      &invalid_symbol, G711_LAW_PCMU, 128, true) &&
		      !v90_pcm_symbol_make(NULL, G711_LAW_PCMU, 78, true) &&
		      memcmp(&invalid_symbol, &saved_symbol,
			     sizeof(invalid_symbol)) == 0,
	      "invalid PCM symbol construction changed output");
	CHECK(strcmp(v90_phase3_sign_result_name(
		     V90_PHASE3_SIGNS_INVALID_STATE), "invalid state") == 0 &&
		      strcmp(v90_phase3_sign_result_name(
			     (enum v90_phase3_sign_result)3), "unknown") == 0,
	      "sign-stream result names are wrong");
	return true;
}

struct test_case {
	const char *name;
	bool (*run)(void);
};

int main(void)
{
	static const struct test_case tests[] = {
		{ "continuous TRN1d, Jd, and Jd-prime", test_continuous_streams },
		{ "Jd handoff boundary", test_jd_boundary },
		{ "validation and transactionality", test_validation },
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
