// SPDX-License-Identifier: BSD-3-Clause

#include <v92_pcm_phase3_initial_tx.h>

#include "test.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static struct v92_pcm_info1a info1a(uint8_t md_units)
{
	return (struct v92_pcm_info1a){
		.filter_sections = V92_PCM_FILTER_P1_Z2,
		.maximum_total_coefficients = 192,
		.maximum_section_coefficients = 128,
		.local_md_length_276_symbol_units = md_units,
		.training_ucode = 103,
	};
}

static bool symbols_match(
	const struct v90_pcm_symbol *left,
	const struct v90_pcm_symbol *right)
{
	return left->ucode == right->ucode &&
	       left->positive == right->positive &&
	       left->octet == right->octet &&
	       left->sample == right->sample;
}

static bool compare_opening(
	struct v92_pcm_phase3_initial_tx *tx,
	struct v92_pcm_phase3_opening *opening,
	const struct v90_pcm_symbol *md_symbol)
{
	bool checked_missing_md = false;

	while (!v92_pcm_phase3_opening_is_complete(opening)) {
		struct v90_pcm_symbol expected;
		struct v90_pcm_symbol actual;
		const struct v90_pcm_symbol *md =
			opening->stage == V92_PCM_PHASE3_OPENING_MD ?
				md_symbol : NULL;

		if (md != NULL && !checked_missing_md) {
			struct v92_pcm_phase3_initial_tx before = *tx;

			memset(&actual, 0xa5, sizeof(actual));
			expected = actual;
			CHECK(v92_pcm_phase3_initial_tx_next(
				      tx, NULL, &actual) ==
				      V92_PCM_PHASE3_INITIAL_TX_NEED_MD_SYMBOL &&
			      memcmp(tx, &before, sizeof(before)) == 0 &&
			      memcmp(&actual, &expected, sizeof(actual)) == 0,
			      "missing MD input was not transactional");
			checked_missing_md = true;
		}
		CHECK(v92_pcm_phase3_opening_next(opening, md, &expected) ==
			      V92_PCM_PHASE3_OPENING_OK &&
		      v92_pcm_phase3_initial_tx_next(tx, md, &actual) ==
			      V92_PCM_PHASE3_INITIAL_TX_OK &&
		      symbols_match(&actual, &expected),
		      "composed opening differed in stage %d",
		      (int)opening->stage);
	}
	CHECK(tx->stage == V92_PCM_PHASE3_INITIAL_TX_TRN1U,
	      "opening did not hand off to TRN1u");
	return true;
}

static bool compare_case(
	enum g711_law law, uint8_t md_units, uint32_t rate_mask,
	uint32_t trn1u_goal, enum v90_dil_profile dil_profile)
{
	struct v92_pcm_info1a selection = info1a(md_units);
	struct v92_pcm_phase3_initial_tx tx;
	struct v92_pcm_phase3_opening opening;
	struct v92_pcm_phase3_trn1u trn1u;
	struct v92_pcm_phase3_ja_tx ja;
	struct v90_pcm_symbol md_symbol;
	struct v92_pcm_dil_parameters parameters = { .upstream_rate_mask = rate_mask };

	CHECK(v90_dil_profile_init(&parameters.downstream, dil_profile) == V90_DIL_OK &&
	      v92_pcm_phase3_initial_tx_init(
		      &tx, law, 91u, &selection, rate_mask, trn1u_goal, dil_profile) ==
		      V92_PCM_PHASE3_INITIAL_TX_OK &&
	      v92_pcm_phase3_initial_tx_is_valid(&tx) &&
	      v92_pcm_phase3_opening_init(
		      &opening, law, 91u, &selection) ==
		      V92_PCM_PHASE3_OPENING_OK &&
	      v90_pcm_symbol_make(&md_symbol, law, 17u, false),
	      "initial stream did not initialize");
	CHECK(compare_opening(&tx, &opening, &md_symbol),
	      "opening comparison failed");
	CHECK(v92_pcm_phase3_trn1u_init(&trn1u, law, 91u) ==
		      V92_PCM_PHASE3_TRN1U_OK,
	      "direct TRN1u did not initialize");
	for (uint32_t index = 0; index < trn1u_goal; ++index) {
		struct v90_pcm_symbol expected;
		struct v90_pcm_symbol actual;

		CHECK(v92_pcm_phase3_trn1u_next(&trn1u, &expected) ==
			      V92_PCM_PHASE3_TRN1U_OK &&
		      v92_pcm_phase3_initial_tx_next(&tx, NULL, &actual) ==
			      V92_PCM_PHASE3_INITIAL_TX_OK &&
		      symbols_match(&actual, &expected),
		      "composed TRN1u differed at symbol %u", index);
	}
	CHECK(tx.stage == V92_PCM_PHASE3_INITIAL_TX_JA &&
	      v92_pcm_phase3_ja_tx_init_dil(&ja, &trn1u, &parameters) ==
		      V92_PCM_PHASE3_JA_OK &&
	      v92_pcm_phase3_initial_tx_request_finish(&tx) ==
		      V92_PCM_PHASE3_INITIAL_TX_OK &&
	      tx.stage == V92_PCM_PHASE3_INITIAL_TX_JA,
	      "TRN1u did not hand off to a minimum-length Ja");
	size_t minimum = V92_PCM_PHASE3_JA_PREAMBLE_BITS + ja.descriptor_bits;
	for (size_t index = 0; index < minimum; ++index) {
		struct v90_pcm_symbol expected;
		struct v90_pcm_symbol actual;

		CHECK(v92_pcm_phase3_ja_tx_next(&ja, &expected) ==
			      V92_PCM_PHASE3_JA_OK &&
		      v92_pcm_phase3_initial_tx_next(&tx, NULL, &actual) ==
			      V92_PCM_PHASE3_INITIAL_TX_OK &&
		      symbols_match(&actual, &expected),
		      "composed Ja differed at symbol %zu", index);
	}
	CHECK(v92_pcm_phase3_initial_tx_is_complete(&tx) &&
	      tx.sequence.ja.emitted_symbols ==
		      minimum,
	      "Ja did not stop at its first permitted boundary");
	{
		struct v92_pcm_phase3_initial_tx before = tx;
		struct v90_pcm_symbol actual;
		struct v90_pcm_symbol output_before;

		memset(&actual, 0xa5, sizeof(actual));
		output_before = actual;
		CHECK(v92_pcm_phase3_initial_tx_next(&tx, NULL, &actual) ==
			      V92_PCM_PHASE3_INITIAL_TX_DONE &&
		      memcmp(&tx, &before, sizeof(before)) == 0 &&
		      memcmp(&actual, &output_before, sizeof(actual)) == 0,
		      "completed initial stream was not stable");
	}
	return true;
}

static bool advance_to_ja(struct v92_pcm_phase3_initial_tx *tx)
{
	struct v92_pcm_info1a selection = info1a(0u);
	struct v90_pcm_symbol symbol;

	if (v92_pcm_phase3_initial_tx_init(
		    tx, G711_LAW_PCMU, 91u, &selection, 1u,
		    V92_PCM_PHASE3_TRN1U_MIN_SYMBOLS, V90_DIL_PROFILE_NONE) !=
	    V92_PCM_PHASE3_INITIAL_TX_OK)
		return false;
	while (tx->stage != V92_PCM_PHASE3_INITIAL_TX_JA) {
		if (v92_pcm_phase3_initial_tx_next(tx, NULL, &symbol) !=
		    V92_PCM_PHASE3_INITIAL_TX_OK)
			return false;
	}
	return true;
}

static bool test_finish_boundaries(void)
{
	struct v92_pcm_phase3_initial_tx tx;
	struct v90_pcm_symbol symbol;

	CHECK(advance_to_ja(&tx), "boundary stream did not reach Ja");
	for (size_t index = 0; index < V92_PCM_PHASE3_JA_MIN_SYMBOLS; ++index)
		CHECK(v92_pcm_phase3_initial_tx_next(&tx, NULL, &symbol) ==
			      V92_PCM_PHASE3_INITIAL_TX_OK,
		      "boundary Ja failed at symbol %zu", index);
	CHECK(tx.stage == V92_PCM_PHASE3_INITIAL_TX_JA &&
	      v92_pcm_phase3_ja_tx_can_stop(&tx.sequence.ja) &&
	      v92_pcm_phase3_initial_tx_request_finish(&tx) ==
		      V92_PCM_PHASE3_INITIAL_TX_OK &&
	      v92_pcm_phase3_initial_tx_is_complete(&tx),
	      "finish request did not use an existing aligned boundary");
	return true;
}

static bool test_contracts(void)
{
	struct v92_pcm_info1a selection = info1a(0u);
	struct v92_pcm_phase3_initial_tx tx;
	struct v90_pcm_symbol symbol;
	uint8_t tx_before[sizeof(tx)];
	uint8_t symbol_before[sizeof(symbol)];
	static const char *const stages[] = {
		"opening", "TRN1u", "Ja", "complete",
	};
	static const char *const results[] = {
		"ok", "need MD symbol", "done", "invalid argument",
		"invalid state", "internal error",
	};

	memset(&tx, 0xa5, sizeof(tx));
	memcpy(tx_before, &tx, sizeof(tx));
	CHECK(v92_pcm_phase3_initial_tx_init(
		      NULL, G711_LAW_PCMU, 91u, &selection, 1u,
		      V92_PCM_PHASE3_TRN1U_MIN_SYMBOLS, V90_DIL_PROFILE_NONE) ==
		      V92_PCM_PHASE3_INITIAL_TX_INVALID_ARGUMENT &&
	      v92_pcm_phase3_initial_tx_init(
		      &tx, (enum g711_law)-1, 91u, &selection, 1u,
		      V92_PCM_PHASE3_TRN1U_MIN_SYMBOLS, V90_DIL_PROFILE_NONE) ==
		      V92_PCM_PHASE3_INITIAL_TX_INVALID_ARGUMENT &&
	      v92_pcm_phase3_initial_tx_init(
		      &tx, G711_LAW_PCMU, 0u, &selection, 1u,
		      V92_PCM_PHASE3_TRN1U_MIN_SYMBOLS, V90_DIL_PROFILE_NONE) ==
		      V92_PCM_PHASE3_INITIAL_TX_INVALID_ARGUMENT &&
	      v92_pcm_phase3_initial_tx_init(
		      &tx, G711_LAW_PCMU, 91u, NULL, 1u,
		      V92_PCM_PHASE3_TRN1U_MIN_SYMBOLS, V90_DIL_PROFILE_NONE) ==
		      V92_PCM_PHASE3_INITIAL_TX_INVALID_ARGUMENT &&
	      v92_pcm_phase3_initial_tx_init(
		      &tx, G711_LAW_PCMU, 91u, &selection,
		      UINT32_C(1) << V92_PCM_DIL_MAX_DATA_RATE_NUMBER,
		      V92_PCM_PHASE3_TRN1U_MIN_SYMBOLS, V90_DIL_PROFILE_NONE) ==
		      V92_PCM_PHASE3_INITIAL_TX_INVALID_ARGUMENT &&
	      v92_pcm_phase3_initial_tx_init(
		      &tx, G711_LAW_PCMU, 91u, &selection, 1u,
		      V92_PCM_PHASE3_TRN1U_MIN_SYMBOLS - 1u, V90_DIL_PROFILE_NONE) ==
		      V92_PCM_PHASE3_INITIAL_TX_INVALID_ARGUMENT &&
	      v92_pcm_phase3_initial_tx_init(
		      &tx, G711_LAW_PCMU, 91u, &selection, 1u,
		      V92_PCM_PHASE3_TRN1U_MIN_SYMBOLS + 1u, V90_DIL_PROFILE_NONE) ==
		      V92_PCM_PHASE3_INITIAL_TX_INVALID_ARGUMENT &&
	      v92_pcm_phase3_initial_tx_init(
		      &tx, G711_LAW_PCMU, 91u, &selection, 1u,
		      V92_PCM_PHASE3_TRN1U_MIN_SYMBOLS, (enum v90_dil_profile)-1) ==
		      V92_PCM_PHASE3_INITIAL_TX_INVALID_ARGUMENT &&
	      memcmp(&tx, tx_before, sizeof(tx)) == 0,
	      "invalid initialization changed its destination");

	CHECK(v92_pcm_phase3_initial_tx_init(
		      &tx, G711_LAW_PCMU, 91u, &selection, 1u,
		      V92_PCM_PHASE3_TRN1U_MIN_SYMBOLS, V90_DIL_PROFILE_NONE) ==
		      V92_PCM_PHASE3_INITIAL_TX_OK,
	      "contract stream did not initialize");
	memset(&symbol, 0xa5, sizeof(symbol));
	memcpy(symbol_before, &symbol, sizeof(symbol));
	memcpy(tx_before, &tx, sizeof(tx));
	CHECK(!v92_pcm_phase3_initial_tx_is_valid(NULL) &&
	      !v92_pcm_phase3_initial_tx_is_complete(NULL) &&
	      v92_pcm_phase3_initial_tx_next(NULL, NULL, &symbol) ==
		      V92_PCM_PHASE3_INITIAL_TX_INVALID_ARGUMENT &&
	      v92_pcm_phase3_initial_tx_next(&tx, NULL, NULL) ==
		      V92_PCM_PHASE3_INITIAL_TX_INVALID_ARGUMENT &&
	      v92_pcm_phase3_initial_tx_request_finish(NULL) ==
		      V92_PCM_PHASE3_INITIAL_TX_INVALID_ARGUMENT &&
	      v92_pcm_phase3_initial_tx_request_finish(&tx) ==
		      V92_PCM_PHASE3_INITIAL_TX_INVALID_STATE &&
	      memcmp(&tx, tx_before, sizeof(tx)) == 0 &&
	      memcmp(&symbol, symbol_before, sizeof(symbol)) == 0,
	      "invalid calls were not transactional");

	tx.stage = (enum v92_pcm_phase3_initial_tx_stage)-1;
	memcpy(tx_before, &tx, sizeof(tx));
	CHECK(!v92_pcm_phase3_initial_tx_is_valid(&tx) &&
	      v92_pcm_phase3_initial_tx_next(&tx, NULL, &symbol) ==
		      V92_PCM_PHASE3_INITIAL_TX_INVALID_STATE &&
	      v92_pcm_phase3_initial_tx_request_finish(&tx) ==
		      V92_PCM_PHASE3_INITIAL_TX_INVALID_STATE &&
	      memcmp(&tx, tx_before, sizeof(tx)) == 0 &&
	      memcmp(&symbol, symbol_before, sizeof(symbol)) == 0,
	      "corrupt state was not rejected transactionally");

	for (size_t index = 0; index < ARRAY_LENGTH(stages); ++index)
		CHECK(strcmp(
			      v92_pcm_phase3_initial_tx_stage_name(
				      (enum v92_pcm_phase3_initial_tx_stage)index),
			      stages[index]) == 0,
		      "stage name %zu differed", index);
	for (size_t index = 0; index < ARRAY_LENGTH(results); ++index)
		CHECK(strcmp(
			      v92_pcm_phase3_initial_tx_result_name(
				      (enum v92_pcm_phase3_initial_tx_result)index),
			      results[index]) == 0,
		      "result name %zu differed", index);
	CHECK(strcmp(
		      v92_pcm_phase3_initial_tx_stage_name(
			      (enum v92_pcm_phase3_initial_tx_stage)-1),
		      "unknown") == 0 &&
	      strcmp(
		      v92_pcm_phase3_initial_tx_result_name(
			      (enum v92_pcm_phase3_initial_tx_result)-1),
		      "unknown") == 0,
	      "unknown names differed");
	return true;
}

int main(void)
{
	fprintf(stderr, "[ RUN      ] V.92 PCM-upstream initial Phase 3\n");
	if (!compare_case(
		    G711_LAW_PCMU, 0u, UINT32_C(0x52491),
		    V92_PCM_PHASE3_TRN1U_MIN_SYMBOLS, V90_DIL_PROFILE_NONE) ||
	    !compare_case(
		    G711_LAW_PCMA, 1u, V92_PCM_DIL_RATE_MASK_ALL,
		    V92_PCM_PHASE3_TRN1U_MIN_SYMBOLS +
			    V92_PCM_PHASE3_TRN1U_ALIGNMENT, V90_DIL_PROFILE_NONE) ||
	    !compare_case(G711_LAW_PCMU, 0u, 0xf,
		    V92_PCM_PHASE3_TRN1U_MIN_SYMBOLS, V90_DIL_PROFILE_CONVENTIONAL) ||
	    !compare_case(G711_LAW_PCMA, 1u, V92_PCM_DIL_RATE_MASK_ALL,
		    V92_PCM_PHASE3_TRN1U_MIN_SYMBOLS, V90_DIL_PROFILE_CONVENTIONAL) ||
	    !test_finish_boundaries() || !test_contracts()) {
		fprintf(stderr, "[  FAILED  ] V.92 PCM-upstream initial Phase 3\n");
		return 1;
	}
	fprintf(stderr, "[       OK ] V.92 PCM-upstream initial Phase 3\n");
	return 0;
}
