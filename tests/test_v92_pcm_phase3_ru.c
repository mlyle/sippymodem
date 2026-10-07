// SPDX-License-Identifier: BSD-3-Clause

#include <g711.h>
#include <v92_pcm_phase3_ru.h>

#include "test.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static uint8_t expected_octet(
	enum g711_law law, uint8_t ucode, bool positive)
{
	uint8_t octet = law == G711_LAW_PCMA ?
			(uint8_t)(ucode ^ UINT8_C(0xd5)) :
			(uint8_t)~ucode;

	return positive ? octet : (uint8_t)(octet ^ UINT8_C(0x80));
}

static int16_t expected_sample(enum g711_law law, uint8_t octet)
{
	return law == G711_LAW_PCMU ?
		       g711_decode_pcmu(octet) :
		       g711_decode_pcma(octet);
}

static bool check_sequence(
	enum g711_law law, uint8_t ucode,
	enum v92_pcm_phase3_ru_kind kind)
{
	struct v92_pcm_phase3_ru sequence;
	struct v90_pcm_symbol symbol;
	uint8_t symbol_before[sizeof(symbol)];
	struct v92_pcm_phase3_ru sequence_before;
	size_t total = kind == V92_PCM_PHASE3_RU ?
			       V92_PCM_PHASE3_RU_SYMBOLS :
			       V92_PCM_PHASE3_RU_BAR_SYMBOLS;

	CHECK(v92_pcm_phase3_ru_init(&sequence, law, ucode, kind) ==
		      V92_PCM_PHASE3_RU_OK &&
	      v92_pcm_phase3_ru_is_valid(&sequence) &&
	      v92_pcm_phase3_ru_remaining(&sequence) == total,
	      "Ru sequence did not initialize");
	for (size_t index = 0; index < total; ++index) {
		bool positive = (index % V92_PCM_PHASE3_RU_PATTERN_SYMBOLS < 3u) !=
				(kind == V92_PCM_PHASE3_RU_BAR);
		uint8_t octet = expected_octet(law, ucode, positive);

		CHECK(v92_pcm_phase3_ru_next(&sequence, &symbol) ==
			      V92_PCM_PHASE3_RU_OK &&
		      symbol.ucode == ucode && symbol.positive == positive &&
		      symbol.octet == octet &&
		      symbol.sample == expected_sample(law, octet) &&
		      v92_pcm_phase3_ru_remaining(&sequence) ==
			      total - index - 1u,
		      "Ru sequence differed at symbol %zu", index);
	}
	memset(&symbol, 0xa5, sizeof(symbol));
	memcpy(symbol_before, &symbol, sizeof(symbol));
	sequence_before = sequence;
	CHECK(v92_pcm_phase3_ru_next(&sequence, &symbol) ==
		      V92_PCM_PHASE3_RU_COMPLETE &&
	      memcmp(&symbol, symbol_before, sizeof(symbol)) == 0 &&
	      sequence.law == sequence_before.law &&
	      sequence.kind == sequence_before.kind &&
	      sequence.emitted_symbols == sequence_before.emitted_symbols &&
	      sequence.lu_ucode == sequence_before.lu_ucode &&
	      v92_pcm_phase3_ru_remaining(&sequence) == 0u,
	      "completed Ru sequence was not stable");
	return true;
}

static bool test_all_sequences(void)
{
	static const uint8_t ucodes[] = { 1, 91, 127 };

	for (enum g711_law law = G711_LAW_PCMU;
	     law <= G711_LAW_PCMA; ++law) {
		for (size_t code = 0; code < ARRAY_LENGTH(ucodes); ++code) {
			CHECK(check_sequence(
				      law, ucodes[code], V92_PCM_PHASE3_RU) &&
			      check_sequence(
				      law, ucodes[code], V92_PCM_PHASE3_RU_BAR),
			      "Ru generation failed for law %u Ucode %u",
			      (unsigned int)law, ucodes[code]);
		}
	}
	return true;
}

static bool test_known_wire_images(void)
{
	static const uint8_t positive[] = { 0xa4, 0x8e };
	static const uint8_t negative[] = { 0x24, 0x0e };

	for (enum g711_law law = G711_LAW_PCMU;
	     law <= G711_LAW_PCMA; ++law) {
		for (enum v92_pcm_phase3_ru_kind kind = V92_PCM_PHASE3_RU;
		     kind <= V92_PCM_PHASE3_RU_BAR; ++kind) {
			struct v92_pcm_phase3_ru sequence;

			CHECK(v92_pcm_phase3_ru_init(&sequence, law, 91, kind) ==
				      V92_PCM_PHASE3_RU_OK,
			      "known Ru image did not initialize");
			for (size_t index = 0; index < 12u; ++index) {
				struct v90_pcm_symbol symbol;
				bool positive_symbol =
					(index % 6u < 3u) !=
					(kind == V92_PCM_PHASE3_RU_BAR);

				CHECK(v92_pcm_phase3_ru_next(
					      &sequence, &symbol) ==
					      V92_PCM_PHASE3_RU_OK &&
				      symbol.octet ==
					      (positive_symbol ? positive[law] :
							 negative[law]),
				      "known Ru wire image differed at %zu", index);
			}
		}
	}
	return true;
}

static bool test_contracts(void)
{
	struct v92_pcm_phase3_ru sequence;
	struct v90_pcm_symbol symbol;
	uint8_t before[sizeof(sequence)];
	uint8_t symbol_before[sizeof(symbol)];
	static const struct {
		enum v92_pcm_phase3_ru_result result;
		const char *name;
	} names[] = {
		{ V92_PCM_PHASE3_RU_OK, "ok" },
		{ V92_PCM_PHASE3_RU_COMPLETE, "complete" },
		{ V92_PCM_PHASE3_RU_INVALID_ARGUMENT, "invalid argument" },
		{ V92_PCM_PHASE3_RU_INVALID_STATE, "invalid state" },
	};

	memset(&sequence, 0xa5, sizeof(sequence));
	memcpy(before, &sequence, sizeof(sequence));
	CHECK(v92_pcm_phase3_ru_init(
		      NULL, G711_LAW_PCMU, 91, V92_PCM_PHASE3_RU) ==
		      V92_PCM_PHASE3_RU_INVALID_ARGUMENT &&
	      v92_pcm_phase3_ru_init(
		      &sequence, (enum g711_law)-1, 91,
		      V92_PCM_PHASE3_RU) == V92_PCM_PHASE3_RU_INVALID_ARGUMENT &&
	      v92_pcm_phase3_ru_init(
		      &sequence, G711_LAW_PCMU, 0,
		      V92_PCM_PHASE3_RU) == V92_PCM_PHASE3_RU_INVALID_ARGUMENT &&
	      v92_pcm_phase3_ru_init(
		      &sequence, G711_LAW_PCMU, 128,
		      V92_PCM_PHASE3_RU) == V92_PCM_PHASE3_RU_INVALID_ARGUMENT &&
	      v92_pcm_phase3_ru_init(
		      &sequence, G711_LAW_PCMU, 91,
		      (enum v92_pcm_phase3_ru_kind)-1) ==
		      V92_PCM_PHASE3_RU_INVALID_ARGUMENT &&
	      memcmp(&sequence, before, sizeof(sequence)) == 0,
	      "invalid Ru initialization changed its destination");
	CHECK(v92_pcm_phase3_ru_init(
		      &sequence, G711_LAW_PCMU, 91, V92_PCM_PHASE3_RU) ==
		      V92_PCM_PHASE3_RU_OK,
	      "contract sequence did not initialize");
	sequence.emitted_symbols = V92_PCM_PHASE3_RU_SYMBOLS + 1u;
	memset(&symbol, 0xa5, sizeof(symbol));
	memcpy(before, &sequence, sizeof(sequence));
	memcpy(symbol_before, &symbol, sizeof(symbol));
	CHECK(!v92_pcm_phase3_ru_is_valid(NULL) &&
	      v92_pcm_phase3_ru_remaining(&sequence) == 0u &&
	      v92_pcm_phase3_ru_next(NULL, &symbol) ==
		      V92_PCM_PHASE3_RU_INVALID_ARGUMENT &&
	      v92_pcm_phase3_ru_next(&sequence, NULL) ==
		      V92_PCM_PHASE3_RU_INVALID_ARGUMENT &&
	      v92_pcm_phase3_ru_next(&sequence, &symbol) ==
		      V92_PCM_PHASE3_RU_INVALID_STATE &&
	      memcmp(&sequence, before, sizeof(sequence)) == 0 &&
	      memcmp(&symbol, symbol_before, sizeof(symbol)) == 0,
	      "invalid Ru state was not transactional");
	for (size_t index = 0; index < ARRAY_LENGTH(names); ++index)
		CHECK(strcmp(
			      v92_pcm_phase3_ru_result_name(names[index].result),
			      names[index].name) == 0,
		      "Ru result name %zu differed", index);
	CHECK(strcmp(
		      v92_pcm_phase3_ru_result_name(
			      (enum v92_pcm_phase3_ru_result)-1),
		      "unknown") == 0,
	      "unknown Ru result name differed");
	return true;
}

int main(void)
{
	fprintf(stderr, "[ RUN      ] generated Ru and Ru-bar sequences\n");
	if (!test_all_sequences() || !test_known_wire_images() ||
	    !test_contracts()) {
		fprintf(stderr, "[  FAILED  ] generated Ru and Ru-bar sequences\n");
		return 1;
	}
	fprintf(stderr, "[       OK ] generated Ru and Ru-bar sequences\n");
	return 0;
}
