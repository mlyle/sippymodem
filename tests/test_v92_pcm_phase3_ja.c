// SPDX-License-Identifier: BSD-3-Clause

#include <v92_pcm_phase3_ja.h>

#include "test.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
	TRN1U_EXTRA_SYMBOLS = V92_PCM_PHASE3_TRN1U_ALIGNMENT,
	JA_TEST_SYMBOLS = V92_PCM_PHASE3_JA_MIN_SYMBOLS +
			  V92_PCM_DIL_ZERO_DESCRIPTOR_BITS + 36,
	REFERENCE_SYMBOLS = V92_PCM_PHASE3_TRN1U_MIN_SYMBOLS +
			    TRN1U_EXTRA_SYMBOLS + JA_TEST_SYMBOLS,
};

static struct v92_pcm_info1a info1a(void)
{
	return (struct v92_pcm_info1a){
		.filter_sections = V92_PCM_FILTER_P1_Z2,
		.maximum_total_coefficients = 192,
		.maximum_section_coefficients = 128,
		.training_ucode = 103,
	};
}

static bool complete_opening(
	struct v92_pcm_phase3_opening *opening,
	enum g711_law law, uint8_t lu_ucode)
{
	struct v92_pcm_info1a selection = info1a();

	if (v92_pcm_phase3_opening_init(
		    opening, law, lu_ucode, &selection) !=
	    V92_PCM_PHASE3_OPENING_OK)
		return false;
	while (!v92_pcm_phase3_opening_is_complete(opening)) {
		struct v90_pcm_symbol symbol;

		if (v92_pcm_phase3_opening_next(opening, NULL, &symbol) !=
		    V92_PCM_PHASE3_OPENING_OK)
			return false;
	}
	return true;
}

static uint8_t reference_scramble(
	uint8_t input, size_t index, const uint8_t *history)
{
	uint8_t output = input;

	if (index >= 5u)
		output ^= history[index - 5u];
	if (index >= 23u)
		output ^= history[index - 23u];
	return output;
}

static uint8_t packed_bit(const uint8_t *descriptor, size_t bit)
{
	return (uint8_t)((descriptor[bit / 8u] >>
			  (7u - bit % 8u)) &
			  1u);
}

static uint8_t expected_octet(
	enum g711_law law, uint8_t ucode, bool positive)
{
	uint8_t octet = law == G711_LAW_PCMA ?
			(uint8_t)(ucode ^ UINT8_C(0xd5)) :
			(uint8_t)~ucode;

	return positive ? octet : (uint8_t)(octet ^ UINT8_C(0x80));
}

static bool prepare_trn1u(
	struct v92_pcm_phase3_trn1u *trn1u,
	enum g711_law law, uint8_t lu_ucode, size_t symbols,
	uint8_t *scrambled_history)
{
	struct v92_pcm_phase3_opening opening;

	if (!complete_opening(&opening, law, lu_ucode) ||
	    v92_pcm_phase3_trn1u_init(trn1u, &opening) !=
		    V92_PCM_PHASE3_TRN1U_OK)
		return false;
	for (size_t index = 0; index < symbols; ++index) {
		struct v90_pcm_symbol symbol;
		uint8_t scrambled = reference_scramble(
			1u, index, scrambled_history);

		scrambled_history[index] = scrambled;
		if (v92_pcm_phase3_trn1u_next(trn1u, &symbol) !=
			    V92_PCM_PHASE3_TRN1U_OK ||
		    symbol.positive != (scrambled == 0u))
			return false;
	}
	return true;
}

static bool check_stream(
	enum g711_law law, uint8_t lu_ucode, uint32_t rate_mask,
	size_t trn1u_symbols)
{
	struct v92_pcm_phase3_trn1u trn1u;
	struct v92_pcm_phase3_ja_tx tx;
	uint8_t descriptor[V92_PCM_DIL_ZERO_DESCRIPTOR_BYTES];
	uint8_t history[REFERENCE_SYMBOLS] = { 0 };
	uint8_t differential_memory;

	CHECK(prepare_trn1u(
		      &trn1u, law, lu_ucode, trn1u_symbols, history) &&
	      v92_pcm_phase3_trn1u_can_handoff(&trn1u) &&
	      v92_pcm_dil_encode_zero(
		      rate_mask, descriptor, sizeof(descriptor)) ==
		      V92_PCM_DIL_OK &&
	      v92_pcm_phase3_ja_tx_init(&tx, &trn1u, rate_mask) ==
		      V92_PCM_PHASE3_JA_OK &&
	      v92_pcm_phase3_ja_tx_is_valid(&tx) &&
	      tx.emitted_symbols == 0u &&
	      tx.coding.previous_sign == history[trn1u_symbols - 1u] &&
	      tx.coding.scrambler.history == trn1u.scrambler.history &&
	      memcmp(tx.descriptor, descriptor, sizeof(descriptor)) == 0 &&
	      !v92_pcm_phase3_ja_tx_can_stop(&tx),
	      "Ja did not initialize for law %u LU %u mask 0x%05x",
	      (unsigned int)law, lu_ucode, (unsigned int)rate_mask);
	differential_memory = history[trn1u_symbols - 1u];
	for (size_t ja_index = 0; ja_index < JA_TEST_SYMBOLS; ++ja_index) {
		struct v90_pcm_symbol symbol;
		size_t stream_index = trn1u_symbols + ja_index;
		uint8_t input = ja_index < V92_PCM_PHASE3_JA_PREAMBLE_BITS ?
				1u :
				packed_bit(
					descriptor,
					(ja_index -
					 V92_PCM_PHASE3_JA_PREAMBLE_BITS) %
						V92_PCM_DIL_ZERO_DESCRIPTOR_BITS);
		uint8_t scrambled = reference_scramble(
			input, stream_index, history);
		bool can_stop = ja_index + 1u >=
					V92_PCM_PHASE3_JA_MIN_SYMBOLS &&
				(ja_index + 1u) %
					V92_PCM_PHASE3_JA_ALIGNMENT == 0u;

		history[stream_index] = scrambled;
		differential_memory ^= scrambled;
		CHECK(v92_pcm_phase3_ja_tx_next(&tx, &symbol) ==
			      V92_PCM_PHASE3_JA_OK &&
		      tx.emitted_symbols == ja_index + 1u &&
		      tx.coding.previous_sign == differential_memory &&
		      symbol.ucode == lu_ucode &&
		      symbol.positive == (differential_memory == 0u) &&
		      symbol.octet == expected_octet(
			      law, lu_ucode, differential_memory == 0u) &&
		      v92_pcm_phase3_ja_tx_can_stop(&tx) == can_stop,
		      "Ja differed at symbol %zu for law %u mask 0x%05x",
		      ja_index, (unsigned int)law,
		      (unsigned int)rate_mask);
	}
	return true;
}

static bool test_generated_streams(void)
{
	CHECK(V92_PCM_PHASE3_JA_PREAMBLE_BITS == 24 &&
	      V92_PCM_PHASE3_JA_MIN_SYMBOLS == 300 &&
	      V92_PCM_PHASE3_JA_ALIGNMENT == 12,
	      "Ja dimensions changed");
	CHECK(check_stream(
		      G711_LAW_PCMU, 91u, 0u,
		      V92_PCM_PHASE3_TRN1U_MIN_SYMBOLS),
	      "zero-mask PCMU Ja stream failed");
	CHECK(check_stream(
		      G711_LAW_PCMA, 103u, V92_PCM_DIL_RATE_MASK_ALL,
		      V92_PCM_PHASE3_TRN1U_MIN_SYMBOLS +
			      TRN1U_EXTRA_SYMBOLS),
	      "full-mask PCMA Ja stream failed");
	CHECK(check_stream(
		      G711_LAW_PCMU, 127u, UINT32_C(0x52491),
		      V92_PCM_PHASE3_TRN1U_MIN_SYMBOLS),
	      "sparse-mask PCMU Ja stream failed");
	return true;
}

static bool test_contracts(void)
{
	struct v92_pcm_phase3_trn1u trn1u;
	struct v92_pcm_phase3_ja_tx tx;
	struct v90_pcm_symbol symbol;
	uint8_t history[REFERENCE_SYMBOLS] = { 0 };
	uint8_t tx_before[sizeof(tx)];
	uint8_t symbol_before[sizeof(symbol)];
	static const char *const names[] = {
		"ok", "invalid argument", "invalid state",
	};

	memset(&tx, 0xa5, sizeof(tx));
	memcpy(tx_before, &tx, sizeof(tx));
	CHECK(prepare_trn1u(
		      &trn1u, G711_LAW_PCMU, 91u,
		      V92_PCM_PHASE3_TRN1U_MIN_SYMBOLS - 1u, history) &&
	      v92_pcm_phase3_ja_tx_init(NULL, &trn1u, 1u) ==
		      V92_PCM_PHASE3_JA_INVALID_ARGUMENT &&
	      v92_pcm_phase3_ja_tx_init(&tx, NULL, 1u) ==
		      V92_PCM_PHASE3_JA_INVALID_ARGUMENT &&
	      v92_pcm_phase3_ja_tx_init(&tx, &trn1u, 1u) ==
		      V92_PCM_PHASE3_JA_INVALID_ARGUMENT &&
	      memcmp(&tx, tx_before, sizeof(tx)) == 0,
	      "invalid Ja handoff changed its destination");

	CHECK(v92_pcm_phase3_trn1u_next(&trn1u, &symbol) ==
		      V92_PCM_PHASE3_TRN1U_OK &&
	      v92_pcm_phase3_ja_tx_init(
		      &tx, &trn1u,
		      UINT32_C(1) << V92_PCM_DIL_MAX_DATA_RATE_NUMBER) ==
		      V92_PCM_PHASE3_JA_INVALID_ARGUMENT &&
	      memcmp(&tx, tx_before, sizeof(tx)) == 0 &&
	      v92_pcm_phase3_ja_tx_init(&tx, &trn1u, 1u) ==
		      V92_PCM_PHASE3_JA_OK,
	      "Ja contract instance did not initialize transactionally");

	memset(&symbol, 0xa5, sizeof(symbol));
	memcpy(symbol_before, &symbol, sizeof(symbol));
	tx.descriptor[0] = 0u;
	memcpy(tx_before, &tx, sizeof(tx));
	CHECK(!v92_pcm_phase3_ja_tx_is_valid(NULL) &&
	      !v92_pcm_phase3_ja_tx_can_stop(NULL) &&
	      v92_pcm_phase3_ja_tx_next(NULL, &symbol) ==
		      V92_PCM_PHASE3_JA_INVALID_ARGUMENT &&
	      v92_pcm_phase3_ja_tx_next(&tx, NULL) ==
		      V92_PCM_PHASE3_JA_INVALID_ARGUMENT &&
	      v92_pcm_phase3_ja_tx_next(&tx, &symbol) ==
		      V92_PCM_PHASE3_JA_INVALID_STATE &&
	      memcmp(&tx, tx_before, sizeof(tx)) == 0 &&
	      memcmp(&symbol, symbol_before, sizeof(symbol)) == 0,
	      "invalid Ja state was not transactional");

	CHECK(v92_pcm_phase3_ja_tx_init(&tx, &trn1u, 1u) ==
		      V92_PCM_PHASE3_JA_OK,
	      "Ja overflow fixture did not reset");
	tx.emitted_symbols = UINT64_MAX;
	memcpy(tx_before, &tx, sizeof(tx));
	CHECK(v92_pcm_phase3_ja_tx_next(&tx, &symbol) ==
		      V92_PCM_PHASE3_JA_INVALID_STATE &&
	      memcmp(&tx, tx_before, sizeof(tx)) == 0 &&
	      memcmp(&symbol, symbol_before, sizeof(symbol)) == 0,
	      "Ja overflow was not rejected transactionally");

	for (size_t index = 0; index < ARRAY_LENGTH(names); ++index)
		CHECK(strcmp(
			      v92_pcm_phase3_ja_result_name(
				      (enum v92_pcm_phase3_ja_result)index),
			      names[index]) == 0,
		      "Ja result name %zu differed", index);
	CHECK(strcmp(
		      v92_pcm_phase3_ja_result_name(
			      (enum v92_pcm_phase3_ja_result)-1),
		      "unknown") == 0,
	      "unknown Ja result name differed");
	return true;
}

int main(void)
{
	fprintf(stderr, "[ RUN      ] V.92 PCM-upstream Ja\n");
	if (!test_generated_streams() || !test_contracts()) {
		fprintf(stderr, "[  FAILED  ] V.92 PCM-upstream Ja\n");
		return 1;
	}
	fprintf(stderr, "[       OK ] V.92 PCM-upstream Ja\n");
	return 0;
}
