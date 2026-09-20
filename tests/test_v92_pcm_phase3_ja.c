// SPDX-License-Identifier: BSD-3-Clause

#include <v92_pcm_phase3_ja.h>

#include "test.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
	TRN1U_EXTRA_SYMBOLS = V92_PCM_PHASE3_TRN1U_ALIGNMENT,
	JA_TEST_SYMBOLS = V92_PCM_PHASE3_JA_PREAMBLE_BITS +
			  2 * V92_PCM_DIL_MAX_DESCRIPTOR_BITS + 36,
	REFERENCE_SYMBOLS = V92_PCM_PHASE3_TRN1U_MIN_SYMBOLS +
			    TRN1U_EXTRA_SYMBOLS + JA_TEST_SYMBOLS,
};

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
	if (v92_pcm_phase3_trn1u_init(trn1u, law, lu_ucode) !=
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
	size_t trn1u_symbols, const struct v90_dil_parameters *dil)
{
	struct v92_pcm_phase3_trn1u trn1u;
	struct v92_pcm_phase3_ja_tx tx;
	struct v92_pcm_dil_parameters request = {
		.downstream = { .sign_pattern_bits = 1, .training_pattern_bits = 1 },
		.upstream_rate_mask = rate_mask,
	};
	uint8_t descriptor[V92_PCM_DIL_MAX_DESCRIPTOR_BYTES] = { 0 };
	uint8_t history[REFERENCE_SYMBOLS] = { 0 };
	uint8_t differential_memory;
	size_t bits;

	if (dil != NULL)
		request.downstream = *dil;
	CHECK(prepare_trn1u(
		      &trn1u, law, lu_ucode, trn1u_symbols, history) &&
	      v92_pcm_phase3_trn1u_can_handoff(&trn1u) &&
	      v92_pcm_dil_encode(
		      &request, descriptor, sizeof(descriptor), &bits) ==
		      V92_PCM_DIL_OK &&
	      (dil == NULL ? v92_pcm_phase3_ja_tx_init(&tx, &trn1u, rate_mask) :
		v92_pcm_phase3_ja_tx_init_dil(&tx, &trn1u, &request)) ==
		      V92_PCM_PHASE3_JA_OK &&
	      v92_pcm_phase3_ja_tx_is_valid(&tx) &&
	      tx.emitted_symbols == 0u &&
	      tx.descriptor_bits == bits &&
	      tx.coding.previous_sign == history[trn1u_symbols - 1u] &&
	      tx.coding.scrambler.history == trn1u.scrambler.history &&
	      memcmp(tx.descriptor, descriptor, sizeof(descriptor)) == 0 &&
	      !v92_pcm_phase3_ja_tx_can_stop(&tx),
	      "Ja did not initialize for law %u LU %u mask 0x%05x",
	      (unsigned int)law, lu_ucode, (unsigned int)rate_mask);
	/* The transmitter must retain its own descriptor, not the source object. */
	memset(&request, 0, sizeof(request));
	differential_memory = history[trn1u_symbols - 1u];
	for (size_t ja_index = 0;
	     ja_index < V92_PCM_PHASE3_JA_PREAMBLE_BITS + 2 * bits + 36;
	     ++ja_index) {
		struct v90_pcm_symbol symbol;
		size_t stream_index = trn1u_symbols + ja_index;
		uint8_t input = ja_index < V92_PCM_PHASE3_JA_PREAMBLE_BITS ?
				1u :
				packed_bit(
					descriptor,
					(ja_index -
					 V92_PCM_PHASE3_JA_PREAMBLE_BITS) %
						bits);
		uint8_t scrambled = reference_scramble(
			input, stream_index, history);
		bool can_stop = ja_index + 1u >=
					V92_PCM_PHASE3_JA_PREAMBLE_BITS + bits &&
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
		      V92_PCM_PHASE3_TRN1U_MIN_SYMBOLS, NULL),
	      "zero-mask PCMU Ja stream failed");
	CHECK(check_stream(
		      G711_LAW_PCMA, 103u, V92_PCM_DIL_RATE_MASK_ALL,
		      V92_PCM_PHASE3_TRN1U_MIN_SYMBOLS +
			      TRN1U_EXTRA_SYMBOLS, NULL),
	      "full-mask PCMA Ja stream failed");
	CHECK(check_stream(
		      G711_LAW_PCMU, 127u, UINT32_C(0x52491),
		      V92_PCM_PHASE3_TRN1U_MIN_SYMBOLS, NULL),
	      "sparse-mask PCMU Ja stream failed");
	return true;
}

static bool test_nonzero_streams(void)
{
	struct v90_dil_parameters dil;
	static const unsigned int segments[] = { 1, 192, 255 };
	static const enum g711_law laws[] = { G711_LAW_PCMU, G711_LAW_PCMA };

	CHECK(v90_dil_profile_init(&dil, V90_DIL_PROFILE_CONVENTIONAL) == V90_DIL_OK,
	      "DIL stream parameters did not initialize");
	for (size_t n = 0; n < ARRAY_LENGTH(segments); ++n) {
		dil.segment_count = (uint8_t)segments[n];
		dil.sign_pattern_bits = n == 2 ? 128 : 120;
		dil.training_pattern_bits = dil.sign_pattern_bits;
		for (size_t i = 0; i < sizeof(dil.training_ucodes); ++i)
			dil.training_ucodes[i] = i < segments[n] ? (uint8_t)(i % 128) : 0;
		for (size_t law = 0; law < ARRAY_LENGTH(laws); ++law)
			CHECK(check_stream(laws[law], 78, 0xf,
				      V92_PCM_PHASE3_TRN1U_MIN_SYMBOLS, &dil),
			      "nonzero Ja failed: law=%u N=%u", laws[law], segments[n]);
	}
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
	{
		struct v92_pcm_dil_parameters invalid = { .upstream_rate_mask = 1 };

		memcpy(tx_before, &tx, sizeof(tx));
		CHECK(v92_pcm_phase3_ja_tx_init_dil(NULL, &trn1u, &invalid) ==
			      V92_PCM_PHASE3_JA_INVALID_ARGUMENT &&
		      v92_pcm_phase3_ja_tx_init_dil(&tx, NULL, &invalid) ==
			      V92_PCM_PHASE3_JA_INVALID_ARGUMENT &&
		      v92_pcm_phase3_ja_tx_init_dil(&tx, &trn1u, NULL) ==
			      V92_PCM_PHASE3_JA_INVALID_ARGUMENT &&
		      v92_pcm_phase3_ja_tx_init_dil(&tx, &trn1u, &invalid) ==
			      V92_PCM_PHASE3_JA_INVALID_ARGUMENT &&
		      memcmp(&tx, tx_before, sizeof(tx)) == 0,
		      "invalid DIL request changed the Ja transmitter");
	}

	memset(&symbol, 0xa5, sizeof(symbol));
	memcpy(symbol_before, &symbol, sizeof(symbol));
	{
		static const size_t lengths[] = { 0, 275, 277,
			V92_PCM_DIL_MAX_DESCRIPTOR_BITS + 12, SIZE_MAX };

		for (size_t i = 0; i < ARRAY_LENGTH(lengths); ++i) {
			tx.descriptor_bits = lengths[i];
			memcpy(tx_before, &tx, sizeof(tx));
			CHECK(!v92_pcm_phase3_ja_tx_is_valid(&tx) &&
			      !v92_pcm_phase3_ja_tx_can_stop(&tx) &&
			      v92_pcm_phase3_ja_tx_next(&tx, &symbol) ==
				      V92_PCM_PHASE3_JA_INVALID_STATE &&
			      memcmp(&tx, tx_before, sizeof(tx)) == 0 &&
			      memcmp(&symbol, symbol_before, sizeof(symbol)) == 0,
			      "invalid Ja length %zu was not transactional", lengths[i]);
		}
		tx.descriptor_bits = V92_PCM_DIL_ZERO_DESCRIPTOR_BITS;
	}
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
	if (!test_generated_streams() || !test_nonzero_streams() || !test_contracts()) {
		fprintf(stderr, "[  FAILED  ] V.92 PCM-upstream Ja\n");
		return 1;
	}
	fprintf(stderr, "[       OK ] V.92 PCM-upstream Ja\n");
	return 0;
}
