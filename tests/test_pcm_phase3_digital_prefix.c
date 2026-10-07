// SPDX-License-Identifier: BSD-3-Clause

#include "test.h"

#include "g711.h"
#include "pcm_phase3_digital_prefix.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct reference_scrambler {
	uint8_t history[V34_SCRAMBLER_BITS];
};

static uint8_t scramble_one(struct reference_scrambler *scrambler)
{
	uint8_t output = 1u ^ scrambler->history[17] ^
			 scrambler->history[22];

	for (size_t delay = V34_SCRAMBLER_BITS - 1u; delay > 0; --delay)
		scrambler->history[delay] = scrambler->history[delay - 1u];
	scrambler->history[0] = output;
	return output;
}

static bool symbol_matches(
	const struct v90_pcm_symbol *symbol, enum g711_law law,
	uint8_t ucode, bool positive)
{
	uint8_t octet;

	return g711_ucode_octet(law, ucode, positive, &octet) &&
	       symbol->ucode == ucode && symbol->positive == positive &&
	       symbol->octet == octet &&
	       (law == G711_LAW_PCMU ? g711_encode_pcmu(symbol->sample) :
					 g711_encode_pcma(symbol->sample)) == octet;
}

static bool run_prefix(enum g711_law law, uint32_t trn1d_symbols)
{
	const uint8_t training_ucode = 78;
	const uint8_t sd_ucode = (uint8_t)(training_ucode + 16u);
	struct pcm_phase3_digital_prefix prefix;
	struct reference_scrambler reference = { 0 };
	struct v90_phase3_signs handoff;
	uint8_t final_sign = 0;

	CHECK(pcm_phase3_digital_prefix_init(
		      &prefix, law, training_ucode, trn1d_symbols) ==
		      PCM_PHASE3_DIGITAL_PREFIX_OK &&
	      prefix.stage == PCM_PHASE3_DIGITAL_PREFIX_SD,
	      "shared prefix initialization failed for law %u",
	      (unsigned int)law);
	for (size_t index = 0; index < V90_PHASE3_SD_SYMBOLS; ++index) {
		struct v90_pcm_symbol symbol;
		size_t phase = index % V90_PHASE3_SD_PATTERN_SYMBOLS;
		uint8_t ucode = phase == 1u || phase == 4u ? 0 : sd_ucode;

		CHECK(pcm_phase3_digital_prefix_next(&prefix, &symbol) ==
			      PCM_PHASE3_DIGITAL_PREFIX_OK &&
		      symbol_matches(&symbol, law, ucode, phase < 3u),
		      "Sd mismatch for law %u at symbol %zu",
		      (unsigned int)law, index);
	}
	CHECK(prefix.stage == PCM_PHASE3_DIGITAL_PREFIX_SD_BAR,
	      "Sd did not hand off for law %u", (unsigned int)law);
	for (size_t index = 0; index < V90_PHASE3_SD_BAR_SYMBOLS; ++index) {
		struct v90_pcm_symbol symbol;
		size_t phase = index % V90_PHASE3_SD_PATTERN_SYMBOLS;
		uint8_t ucode = phase == 1u || phase == 4u ? 0 : sd_ucode;

		CHECK(pcm_phase3_digital_prefix_next(&prefix, &symbol) ==
			      PCM_PHASE3_DIGITAL_PREFIX_OK &&
		      symbol_matches(&symbol, law, ucode, phase >= 3u),
		      "Sd-bar mismatch for law %u at symbol %zu",
		      (unsigned int)law, index);
	}
	CHECK(prefix.stage == PCM_PHASE3_DIGITAL_PREFIX_TRN1D,
	      "Sd-bar did not hand off for law %u", (unsigned int)law);
	for (size_t index = 0;
	     index < trn1d_symbols; ++index) {
		struct v90_pcm_symbol symbol;
		uint8_t sign = scramble_one(&reference);

		CHECK(pcm_phase3_digital_prefix_next(&prefix, &symbol) ==
			      PCM_PHASE3_DIGITAL_PREFIX_OK &&
		      symbol_matches(
			      &symbol, law, training_ucode, sign != 0),
		      "TRN1d mismatch for law %u at symbol %zu",
		      (unsigned int)law, index);
		final_sign = sign;
	}
	CHECK(pcm_phase3_digital_prefix_is_complete(&prefix) &&
	      pcm_phase3_digital_prefix_handoff(&prefix, &handoff) ==
		      PCM_PHASE3_DIGITAL_PREFIX_OK &&
	      handoff.phase == V90_PHASE3_SIGNS_TRN1D &&
	      handoff.scrambler.history ==
		      prefix.sequence.signs.scrambler.history &&
	      handoff.trn1d_symbols == trn1d_symbols &&
	      handoff.last_sign == final_sign,
	      "shared prefix handoff changed coding state for law %u",
	      (unsigned int)law);
	{
		struct pcm_phase3_digital_prefix saved = prefix;
		struct v90_pcm_symbol symbol = { .octet = 0xa5 };
		struct v90_pcm_symbol saved_symbol = symbol;

		CHECK(pcm_phase3_digital_prefix_next(&prefix, &symbol) ==
			      PCM_PHASE3_DIGITAL_PREFIX_DONE &&
		      memcmp(&prefix, &saved, sizeof(saved)) == 0 &&
		      memcmp(&symbol, &saved_symbol, sizeof(symbol)) == 0,
		      "completed prefix changed state or output");
	}
	return true;
}

static bool test_exact_prefix(void)
{
	return run_prefix(G711_LAW_PCMU, V90_PHASE3_TRN1D_MIN_SYMBOLS) &&
	       run_prefix(G711_LAW_PCMA, V90_PHASE3_TRN1D_MIN_SYMBOLS) &&
	       run_prefix(G711_LAW_PCMU, V90_PHASE3_TRN1D_SYMBOLS) &&
	       run_prefix(G711_LAW_PCMA, V90_PHASE3_TRN1D_SYMBOLS);
}

static bool test_validation(void)
{
	struct pcm_phase3_digital_prefix prefix;
	struct pcm_phase3_digital_prefix saved;
	struct v90_phase3_signs signs;
	struct v90_phase3_signs saved_signs;
	struct v90_pcm_symbol symbol = { .sample = 1234, .octet = 0xa5 };
	struct v90_pcm_symbol saved_symbol = symbol;

	memset(&prefix, 0xa5, sizeof(prefix));
	saved = prefix;
	CHECK(pcm_phase3_digital_prefix_init(
		      NULL, G711_LAW_PCMU, 78,
		      V90_PHASE3_TRN1D_MIN_SYMBOLS) ==
		      PCM_PHASE3_DIGITAL_PREFIX_INVALID_ARGUMENT &&
	      pcm_phase3_digital_prefix_init(
		      &prefix, (enum g711_law)2, 78,
		      V90_PHASE3_TRN1D_MIN_SYMBOLS) ==
		      PCM_PHASE3_DIGITAL_PREFIX_INVALID_ARGUMENT &&
	      pcm_phase3_digital_prefix_init(
		      &prefix, G711_LAW_PCMU, 66,
		      V90_PHASE3_TRN1D_MIN_SYMBOLS) ==
		      PCM_PHASE3_DIGITAL_PREFIX_INVALID_ARGUMENT &&
	      pcm_phase3_digital_prefix_init(
		      &prefix, G711_LAW_PCMU, 78,
		      V90_PHASE3_TRN1D_MIN_SYMBOLS + 1u) ==
		      PCM_PHASE3_DIGITAL_PREFIX_INVALID_ARGUMENT &&
	      memcmp(&prefix, &saved, sizeof(saved)) == 0,
	      "invalid prefix initialization changed state");
	CHECK(pcm_phase3_digital_prefix_init(
		      &prefix, G711_LAW_PCMU, 78,
		      V90_PHASE3_TRN1D_MIN_SYMBOLS) ==
		      PCM_PHASE3_DIGITAL_PREFIX_OK,
	      "valid prefix fixture did not initialize");
	memset(&signs, 0xa5, sizeof(signs));
	saved_signs = signs;
	CHECK(pcm_phase3_digital_prefix_handoff(&prefix, &signs) ==
		      PCM_PHASE3_DIGITAL_PREFIX_INVALID_STATE &&
	      memcmp(&signs, &saved_signs, sizeof(signs)) == 0 &&
	      pcm_phase3_digital_prefix_next(NULL, &symbol) ==
		      PCM_PHASE3_DIGITAL_PREFIX_INVALID_ARGUMENT &&
	      pcm_phase3_digital_prefix_next(&prefix, NULL) ==
		      PCM_PHASE3_DIGITAL_PREFIX_INVALID_ARGUMENT &&
	      memcmp(&symbol, &saved_symbol, sizeof(symbol)) == 0,
	      "invalid prefix operations were not transactional");
	prefix.stage = (enum pcm_phase3_digital_prefix_stage)99;
	CHECK(!pcm_phase3_digital_prefix_is_valid(&prefix) &&
	      !pcm_phase3_digital_prefix_is_complete(NULL) &&
	      pcm_phase3_digital_prefix_handoff(NULL, &signs) ==
		      PCM_PHASE3_DIGITAL_PREFIX_INVALID_ARGUMENT &&
	      pcm_phase3_digital_prefix_handoff(&prefix, NULL) ==
		      PCM_PHASE3_DIGITAL_PREFIX_INVALID_ARGUMENT,
	      "prefix validation changed");
	return true;
}

struct test_case {
	const char *name;
	bool (*run)(void);
};

int main(void)
{
	static const struct test_case tests[] = {
		{ "exact shared prefix", test_exact_prefix },
		{ "validation", test_validation },
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
