// SPDX-License-Identifier: BSD-3-Clause

#include <v92_pcm_phase3_trn1u.h>

#include "test.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
	TEST_SYMBOLS = V92_PCM_PHASE3_TRN1U_MIN_SYMBOLS +
		       V92_PCM_PHASE3_TRN1U_ALIGNMENT,
};

static uint8_t expected_octet(
	enum g711_law law, uint8_t ucode, bool positive)
{
	uint8_t octet = law == G711_LAW_PCMA ?
			(uint8_t)(ucode ^ UINT8_C(0xd5)) :
			(uint8_t)~ucode;

	return positive ? octet : (uint8_t)(octet ^ UINT8_C(0x80));
}

static bool check_sequence(enum g711_law law, uint8_t lu_ucode)
{
	struct v92_pcm_phase3_trn1u trn1u;
	uint8_t bits[TEST_SYMBOLS] = { 0 };

	CHECK(v92_pcm_phase3_trn1u_init(&trn1u, law, lu_ucode) ==
		      V92_PCM_PHASE3_TRN1U_OK &&
	      v92_pcm_phase3_trn1u_is_valid(&trn1u) &&
	      trn1u.scrambler.polynomial == V34_POLYNOMIAL_GPA &&
	      trn1u.scrambler.history == 0u &&
	      trn1u.emitted_symbols == 0u &&
	      !v92_pcm_phase3_trn1u_can_handoff(&trn1u),
	      "TRN1u did not initialize for law %u LU %u",
	      (unsigned int)law, lu_ucode);
	for (size_t index = 0; index < TEST_SYMBOLS; ++index) {
		struct v90_pcm_symbol symbol;
		uint8_t expected = 1u;

		if (index >= 5u)
			expected ^= bits[index - 5u];
		if (index >= 23u)
			expected ^= bits[index - 23u];
		bits[index] = expected;
		CHECK(v92_pcm_phase3_trn1u_next(&trn1u, &symbol) ==
			      V92_PCM_PHASE3_TRN1U_OK &&
		      trn1u.emitted_symbols == index + 1u &&
		      symbol.ucode == lu_ucode &&
		      symbol.positive == (expected == 0u) &&
		      symbol.octet == expected_octet(
			      law, lu_ucode, expected == 0u) &&
		      v92_pcm_phase3_trn1u_can_handoff(&trn1u) ==
			      (index + 1u >=
				       V92_PCM_PHASE3_TRN1U_MIN_SYMBOLS &&
			       (index + 1u) %
				       V92_PCM_PHASE3_TRN1U_ALIGNMENT == 0u),
		      "TRN1u differed at symbol %zu", index);
	}
	return true;
}

static bool test_sequences(void)
{
	static const uint8_t lu_ucodes[] = { 1, 91, 127 };

	for (enum g711_law law = G711_LAW_PCMU;
	     law <= G711_LAW_PCMA; ++law) {
		for (size_t code = 0; code < ARRAY_LENGTH(lu_ucodes); ++code)
			CHECK(check_sequence(law, lu_ucodes[code]),
			      "TRN1u sequence failed");
	}
	return true;
}

static bool test_contracts(void)
{
	struct v92_pcm_phase3_trn1u trn1u;
	struct v90_pcm_symbol symbol;
	uint8_t trn1u_before[sizeof(trn1u)];
	uint8_t symbol_before[sizeof(symbol)];
	static const char *const names[] = {
		"ok", "invalid argument", "invalid state",
	};

	memset(&trn1u, 0xa5, sizeof(trn1u));
	memcpy(trn1u_before, &trn1u, sizeof(trn1u));
	CHECK(v92_pcm_phase3_trn1u_init(NULL, G711_LAW_PCMU, 91u) ==
		      V92_PCM_PHASE3_TRN1U_INVALID_ARGUMENT &&
	      v92_pcm_phase3_trn1u_init(
		      &trn1u, (enum g711_law)-1, 91u) ==
		      V92_PCM_PHASE3_TRN1U_INVALID_ARGUMENT &&
	      v92_pcm_phase3_trn1u_init(&trn1u, G711_LAW_PCMU, 0u) ==
		      V92_PCM_PHASE3_TRN1U_INVALID_ARGUMENT &&
	      v92_pcm_phase3_trn1u_init(&trn1u, G711_LAW_PCMU, 128u) ==
		      V92_PCM_PHASE3_TRN1U_INVALID_ARGUMENT &&
	      memcmp(&trn1u, trn1u_before, sizeof(trn1u)) == 0,
	      "invalid TRN1u initialization changed its destination");
	CHECK(v92_pcm_phase3_trn1u_init(
		      &trn1u, G711_LAW_PCMU, 91u) ==
		      V92_PCM_PHASE3_TRN1U_OK,
	      "TRN1u contract instance did not initialize");
	trn1u.emitted_symbols = UINT32_MAX;
	memcpy(trn1u_before, &trn1u, sizeof(trn1u));
	memset(&symbol, 0xa5, sizeof(symbol));
	memcpy(symbol_before, &symbol, sizeof(symbol));
	CHECK(!v92_pcm_phase3_trn1u_is_valid(NULL) &&
	      !v92_pcm_phase3_trn1u_can_handoff(NULL) &&
	      v92_pcm_phase3_trn1u_next(NULL, &symbol) ==
		      V92_PCM_PHASE3_TRN1U_INVALID_ARGUMENT &&
	      v92_pcm_phase3_trn1u_next(&trn1u, NULL) ==
		      V92_PCM_PHASE3_TRN1U_INVALID_ARGUMENT &&
	      v92_pcm_phase3_trn1u_next(&trn1u, &symbol) ==
		      V92_PCM_PHASE3_TRN1U_INVALID_STATE &&
	      memcmp(&trn1u, trn1u_before, sizeof(trn1u)) == 0 &&
	      memcmp(&symbol, symbol_before, sizeof(symbol)) == 0,
	      "invalid TRN1u state was not transactional");
	for (size_t index = 0; index < ARRAY_LENGTH(names); ++index)
		CHECK(strcmp(
			      v92_pcm_phase3_trn1u_result_name(
				      (enum v92_pcm_phase3_trn1u_result)index),
			      names[index]) == 0,
		      "TRN1u result name %zu differed", index);
	CHECK(strcmp(
		      v92_pcm_phase3_trn1u_result_name(
			      (enum v92_pcm_phase3_trn1u_result)-1),
		      "unknown") == 0,
	      "unknown TRN1u result name differed");
	return true;
}

int main(void)
{
	fprintf(stderr, "[ RUN      ] V.92 PCM-upstream TRN1u\n");
	if (!test_sequences() || !test_contracts()) {
		fprintf(stderr, "[  FAILED  ] V.92 PCM-upstream TRN1u\n");
		return 1;
	}
	fprintf(stderr, "[       OK ] V.92 PCM-upstream TRN1u\n");
	return 0;
}
