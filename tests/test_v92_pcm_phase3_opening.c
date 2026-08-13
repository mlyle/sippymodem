// SPDX-License-Identifier: BSD-3-Clause

#include <v92_pcm_phase3_opening.h>

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

static uint8_t expected_octet(
	enum g711_law law, uint8_t ucode, bool positive)
{
	uint8_t octet = law == G711_LAW_PCMA ?
			(uint8_t)(ucode ^ UINT8_C(0xd5)) :
			(uint8_t)~ucode;

	return positive ? octet : (uint8_t)(octet ^ UINT8_C(0x80));
}

static bool symbol_matches(
	const struct v90_pcm_symbol *symbol, enum g711_law law,
	uint8_t ucode, bool positive)
{
	return symbol->ucode == ucode && symbol->positive == positive &&
	       symbol->octet == expected_octet(law, ucode, positive);
}

static enum v92_pcm_phase3_opening_stage expected_stage(
	size_t index, size_t md_symbols)
{
	if (index < 560u)
		return V92_PCM_PHASE3_OPENING_SILENCE;
	if (index < 944u)
		return V92_PCM_PHASE3_OPENING_RU;
	if (index < 968u)
		return V92_PCM_PHASE3_OPENING_RU_BAR;
	if (md_symbols == 0u)
		return V92_PCM_PHASE3_OPENING_TRN1U_READY;
	if (index < 968u + md_symbols)
		return V92_PCM_PHASE3_OPENING_MD;
	if (index < 1352u + md_symbols)
		return V92_PCM_PHASE3_OPENING_SECOND_RU;
	if (index < 1376u + md_symbols)
		return V92_PCM_PHASE3_OPENING_SECOND_RU_BAR;
	return V92_PCM_PHASE3_OPENING_TRN1U_READY;
}

static bool check_case(enum g711_law law, uint8_t md_units)
{
	struct v92_pcm_phase3_opening opening;
	struct v92_pcm_info1a selection = info1a(md_units);
	struct v90_pcm_symbol md_symbol;
	struct v90_pcm_symbol symbol;
	uint8_t opening_before[sizeof(opening)];
	uint8_t symbol_before[sizeof(symbol)];
	size_t md_count = (size_t)md_units * V92_PCM_PHASE3_MD_UNIT_SYMBOLS;
	size_t total = 968u + (md_count == 0u ? 0u : md_count + 408u);

	CHECK(v92_pcm_phase3_opening_init(&opening, law, 91u, &selection) ==
		      V92_PCM_PHASE3_OPENING_OK &&
	      v92_pcm_phase3_opening_is_valid(&opening) &&
	      opening.stage == V92_PCM_PHASE3_OPENING_SILENCE &&
	      v92_pcm_phase3_opening_remaining(&opening) == total,
	      "opening did not initialize for law %u MD %u",
	      (unsigned int)law, md_units);
	CHECK(v90_pcm_symbol_make(&md_symbol, law, 17u, false),
	      "MD fixture did not initialize");
	memcpy(opening_before, &opening, sizeof(opening));
	memset(&symbol, 0xa5, sizeof(symbol));
	memcpy(symbol_before, &symbol, sizeof(symbol));
	CHECK(v92_pcm_phase3_opening_next(
		      &opening, &md_symbol, &symbol) ==
		      V92_PCM_PHASE3_OPENING_INVALID_ARGUMENT &&
	      memcmp(&opening, opening_before, sizeof(opening)) == 0 &&
	      memcmp(&symbol, symbol_before, sizeof(symbol)) == 0,
	      "opening accepted an MD symbol during standardized output");

	for (size_t index = 0; index < total; ++index) {
		enum v92_pcm_phase3_opening_stage stage =
			expected_stage(index, md_count);
		const struct v90_pcm_symbol *provided =
			stage == V92_PCM_PHASE3_OPENING_MD ? &md_symbol : NULL;

		CHECK(opening.stage == stage &&
		      v92_pcm_phase3_opening_remaining(&opening) ==
			      total - index,
		      "opening boundary differed at symbol %zu", index);
		if (stage == V92_PCM_PHASE3_OPENING_MD && index == 968u) {
			memcpy(opening_before, &opening, sizeof(opening));
			memset(&symbol, 0xa5, sizeof(symbol));
			memcpy(symbol_before, &symbol, sizeof(symbol));
			CHECK(v92_pcm_phase3_opening_next(
				      &opening, NULL, &symbol) ==
				      V92_PCM_PHASE3_OPENING_NEED_MD_SYMBOL &&
			      memcmp(&opening, opening_before,
				     sizeof(opening)) == 0 &&
			      memcmp(&symbol, symbol_before, sizeof(symbol)) == 0,
			      "missing MD input was not transactional");
			md_symbol.sample ^= 1;
			CHECK(v92_pcm_phase3_opening_next(
				      &opening, &md_symbol, &symbol) ==
				      V92_PCM_PHASE3_OPENING_INVALID_ARGUMENT &&
			      memcmp(&opening, opening_before,
				     sizeof(opening)) == 0 &&
			      memcmp(&symbol, symbol_before, sizeof(symbol)) == 0,
			      "malformed MD input was not rejected");
			md_symbol.sample ^= 1;
		}
		CHECK(v92_pcm_phase3_opening_next(
			      &opening, provided, &symbol) ==
			      V92_PCM_PHASE3_OPENING_OK &&
		      v92_pcm_phase3_opening_remaining(&opening) ==
			      total - index - 1u,
		      "opening failed at symbol %zu", index);
		if (stage == V92_PCM_PHASE3_OPENING_SILENCE) {
			CHECK(symbol_matches(&symbol, law, 0u, true),
			      "silence differed at symbol %zu", index);
		} else if (stage == V92_PCM_PHASE3_OPENING_MD) {
			CHECK(symbol.ucode == md_symbol.ucode &&
			      symbol.positive == md_symbol.positive &&
			      symbol.octet == md_symbol.octet &&
			      symbol.sample == md_symbol.sample,
			      "MD symbol was not passed through at %zu", index);
		} else {
			size_t phase = stage == V92_PCM_PHASE3_OPENING_RU ?
					       index - 560u :
				       stage == V92_PCM_PHASE3_OPENING_RU_BAR ?
					       index - 944u :
				       stage ==
					       V92_PCM_PHASE3_OPENING_SECOND_RU ?
					       index - 968u - md_count :
					       index - 1352u - md_count;
			bool inverted =
				stage == V92_PCM_PHASE3_OPENING_RU_BAR ||
				stage == V92_PCM_PHASE3_OPENING_SECOND_RU_BAR;
			bool positive = (phase % 6u < 3u) != inverted;

			CHECK(symbol_matches(&symbol, law, 91u, positive),
			      "Ru span differed at symbol %zu", index);
		}
	}
	CHECK(v92_pcm_phase3_opening_is_complete(&opening) &&
	      opening.stage == V92_PCM_PHASE3_OPENING_TRN1U_READY,
	      "opening did not stop at the TRN1u handoff");
	memcpy(opening_before, &opening, sizeof(opening));
	memset(&symbol, 0xa5, sizeof(symbol));
	memcpy(symbol_before, &symbol, sizeof(symbol));
	CHECK(v92_pcm_phase3_opening_next(&opening, NULL, &symbol) ==
		      V92_PCM_PHASE3_OPENING_COMPLETE &&
	      memcmp(&opening, opening_before, sizeof(opening)) == 0 &&
	      memcmp(&symbol, symbol_before, sizeof(symbol)) == 0,
	      "completed opening was not stable");
	return true;
}

static bool test_timelines(void)
{
	static const uint8_t md_units[] = { 0, 1, 127 };

	for (enum g711_law law = G711_LAW_PCMU;
	     law <= G711_LAW_PCMA; ++law) {
		for (size_t index = 0; index < ARRAY_LENGTH(md_units); ++index)
			CHECK(check_case(law, md_units[index]),
			      "opening case failed");
	}
	return true;
}

static bool test_contracts(void)
{
	struct v92_pcm_phase3_opening opening;
	struct v92_pcm_info1a selection = info1a(0);
	struct v90_pcm_symbol symbol;
	uint8_t opening_before[sizeof(opening)];
	uint8_t symbol_before[sizeof(symbol)];
	static const char *const stages[] = {
		"silence", "Ru", "Ru-bar", "MD", "second Ru",
		"second Ru-bar", "TRN1u ready",
	};
	static const char *const results[] = {
		"ok", "need MD symbol", "complete", "invalid argument",
		"invalid state",
	};

	memset(&opening, 0xa5, sizeof(opening));
	memcpy(opening_before, &opening, sizeof(opening));
	CHECK(v92_pcm_phase3_opening_init(
		      NULL, G711_LAW_PCMU, 91u, &selection) ==
		      V92_PCM_PHASE3_OPENING_INVALID_ARGUMENT &&
	      v92_pcm_phase3_opening_init(
		      &opening, (enum g711_law)-1, 91u, &selection) ==
		      V92_PCM_PHASE3_OPENING_INVALID_ARGUMENT &&
	      v92_pcm_phase3_opening_init(
		      &opening, G711_LAW_PCMU, 91u, NULL) ==
		      V92_PCM_PHASE3_OPENING_INVALID_ARGUMENT &&
	      v92_pcm_phase3_opening_init(
		      &opening, G711_LAW_PCMU, 0u, &selection) ==
		      V92_PCM_PHASE3_OPENING_INVALID_ARGUMENT,
	      "invalid opening initialization was accepted");
	selection.maximum_total_coefficients = 128;
	CHECK(v92_pcm_phase3_opening_init(
		      &opening, G711_LAW_PCMU, 91u, &selection) ==
		      V92_PCM_PHASE3_OPENING_INVALID_ARGUMENT &&
	      memcmp(&opening, opening_before, sizeof(opening)) == 0,
	      "invalid opening initialization changed its destination");
	selection = info1a(0);
	CHECK(v92_pcm_phase3_opening_init(
		      &opening, G711_LAW_PCMU, 91u, &selection) ==
		      V92_PCM_PHASE3_OPENING_OK,
	      "contract opening did not initialize");
	opening.state.emitted_symbols =
		V92_PCM_PHASE3_OPENING_SILENCE_SYMBOLS;
	memcpy(opening_before, &opening, sizeof(opening));
	memset(&symbol, 0xa5, sizeof(symbol));
	memcpy(symbol_before, &symbol, sizeof(symbol));
	CHECK(!v92_pcm_phase3_opening_is_valid(NULL) &&
	      !v92_pcm_phase3_opening_is_valid(&opening) &&
	      v92_pcm_phase3_opening_remaining(&opening) == 0u &&
	      v92_pcm_phase3_opening_next(NULL, NULL, &symbol) ==
		      V92_PCM_PHASE3_OPENING_INVALID_ARGUMENT &&
	      v92_pcm_phase3_opening_next(&opening, NULL, NULL) ==
		      V92_PCM_PHASE3_OPENING_INVALID_ARGUMENT &&
	      v92_pcm_phase3_opening_next(&opening, NULL, &symbol) ==
		      V92_PCM_PHASE3_OPENING_INVALID_STATE &&
	      memcmp(&opening, opening_before, sizeof(opening)) == 0 &&
	      memcmp(&symbol, symbol_before, sizeof(symbol)) == 0,
	      "invalid opening state was not transactional");
	for (size_t index = 0; index < ARRAY_LENGTH(stages); ++index)
		CHECK(strcmp(
			      v92_pcm_phase3_opening_stage_name(
				      (enum v92_pcm_phase3_opening_stage)index),
			      stages[index]) == 0,
		      "opening stage name %zu differed", index);
	for (size_t index = 0; index < ARRAY_LENGTH(results); ++index)
		CHECK(strcmp(
			      v92_pcm_phase3_opening_result_name(
				      (enum v92_pcm_phase3_opening_result)index),
			      results[index]) == 0,
		      "opening result name %zu differed", index);
	CHECK(strcmp(
		      v92_pcm_phase3_opening_stage_name(
			      (enum v92_pcm_phase3_opening_stage)-1),
		      "unknown") == 0 &&
	      strcmp(
		      v92_pcm_phase3_opening_result_name(
			      (enum v92_pcm_phase3_opening_result)-1),
		      "unknown") == 0,
	      "unknown opening names differed");
	return true;
}

int main(void)
{
	fprintf(stderr, "[ RUN      ] V.92 PCM-upstream Phase 3 opening\n");
	if (!test_timelines() || !test_contracts()) {
		fprintf(stderr, "[  FAILED  ] V.92 PCM-upstream Phase 3 opening\n");
		return 1;
	}
	fprintf(stderr, "[       OK ] V.92 PCM-upstream Phase 3 opening\n");
	return 0;
}
