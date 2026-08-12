// SPDX-License-Identifier: BSD-3-Clause

#include "test.h"

#include "g711.h"
#include "v90_info1.h"
#include "v90_phase3_sd.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static uint8_t expected_octet(
	enum g711_law law, uint8_t ucode, bool positive)
{
	uint8_t octet = law == G711_LAW_PCMA ?
				(uint8_t)(ucode ^ 0xd5u) :
				(uint8_t)~ucode;

	return positive ? octet : (uint8_t)(octet ^ 0x80u);
}

static uint8_t encode_sample(enum g711_law law, int16_t sample)
{
	return law == G711_LAW_PCMA ? g711_encode_pcma(sample) :
					   g711_encode_pcmu(sample);
}

static bool check_sequence(
	enum g711_law law, enum v90_phase3_sd_kind kind,
	uint8_t training_ucode)
{
	struct v90_phase3_sd sequence;
	struct v90_pcm_symbol sentinel = {
		.ucode = 0xa5,
		.positive = true,
		.octet = 0x5a,
		.sample = 1234,
	};
	struct v90_pcm_symbol unchanged = sentinel;
	size_t count = kind == V90_PHASE3_SD ?
			       V90_PHASE3_SD_SYMBOLS :
			       V90_PHASE3_SD_BAR_SYMBOLS;
	uint8_t w_ucode = (uint8_t)((training_ucode + 16u) & 0x7fu);

	CHECK(v90_phase3_sd_init(&sequence, law, training_ucode, kind) ==
		      V90_PHASE3_SD_OK &&
		      v90_phase3_sd_remaining(&sequence) == count,
	      "Sd initialization failed for law %u kind %u",
	      (unsigned int)law, (unsigned int)kind);
	for (size_t index = 0; index < count; ++index) {
		struct v90_pcm_symbol symbol = sentinel;
		unsigned int phase = index % 6u;
		uint8_t ucode = phase == 1 || phase == 4 ? 0 : w_ucode;
		bool positive = (phase < 3) != (kind == V90_PHASE3_SD_BAR);

		CHECK(v90_phase3_sd_next(&sequence, &symbol) ==
			      V90_PHASE3_SD_OK,
		      "Sd generation failed at symbol %zu", index);
		CHECK(symbol.ucode == ucode && symbol.positive == positive &&
			      symbol.octet == expected_octet(law, ucode, positive),
		      "Sd symbol %zu has the wrong code or polarity", index);
		CHECK(encode_sample(law, symbol.sample) == symbol.octet,
		      "Sd symbol %zu lost its exact network octet", index);
		CHECK(v90_phase3_sd_remaining(&sequence) == count - index - 1,
		      "Sd remaining count is wrong at symbol %zu", index);
	}
	CHECK(v90_phase3_sd_next(&sequence, &sentinel) ==
		      V90_PHASE3_SD_COMPLETE &&
		      memcmp(&sentinel, &unchanged, sizeof(sentinel)) == 0 &&
		      sequence.emitted_symbols == count,
	      "completed Sd sequence changed its state or output");
	return true;
}

static bool test_exact_sequences(void)
{
	for (enum g711_law law = G711_LAW_PCMU;
	     law <= G711_LAW_PCMA; ++law) {
		CHECK(check_sequence(law, V90_PHASE3_SD, 91),
		      "Sd failed for law %u", (unsigned int)law);
		CHECK(check_sequence(law, V90_PHASE3_SD_BAR, 91),
		      "Sd-bar failed for law %u", (unsigned int)law);
	}
	return true;
}

static bool test_training_code_boundaries(void)
{
	CHECK(check_sequence(
		      G711_LAW_PCMU, V90_PHASE3_SD,
		      V90_INFO1A_MIN_TRAINING_UCODE) &&
		      check_sequence(
			      G711_LAW_PCMA, V90_PHASE3_SD_BAR,
			      V90_INFO1A_MAX_TRAINING_UCODE),
	      "Sd rejected a legal training-code boundary");
	return true;
}

static bool test_validation(void)
{
	struct v90_phase3_sd sequence;
	struct v90_phase3_sd saved;
	struct v90_pcm_symbol symbol = {
		.ucode = 0xa5,
		.octet = 0x5a,
		.sample = 1234,
	};
	struct v90_pcm_symbol saved_symbol = symbol;

	memset(&sequence, 0xa5, sizeof(sequence));
	saved = sequence;
	CHECK(v90_phase3_sd_init(
		      &sequence, G711_LAW_PCMU,
		      V90_INFO1A_MIN_TRAINING_UCODE - 1u,
		      V90_PHASE3_SD) == V90_PHASE3_SD_INVALID_ARGUMENT &&
		      v90_phase3_sd_init(
			      &sequence, G711_LAW_PCMU,
			      V90_INFO1A_MAX_TRAINING_UCODE + 1u,
			      V90_PHASE3_SD) ==
			      V90_PHASE3_SD_INVALID_ARGUMENT &&
		      v90_phase3_sd_init(
			      &sequence, (enum g711_law)2, 91,
			      V90_PHASE3_SD) == V90_PHASE3_SD_INVALID_ARGUMENT &&
		      v90_phase3_sd_init(
			      &sequence, G711_LAW_PCMU, 91,
			      (enum v90_phase3_sd_kind)2) ==
			      V90_PHASE3_SD_INVALID_ARGUMENT &&
		      v90_phase3_sd_init(
			      NULL, G711_LAW_PCMU, 91,
			      V90_PHASE3_SD) == V90_PHASE3_SD_INVALID_ARGUMENT &&
		      memcmp(&sequence, &saved, sizeof(sequence)) == 0,
	      "invalid Sd initialization changed output state");
	CHECK(v90_phase3_sd_next(NULL, &symbol) ==
		      V90_PHASE3_SD_INVALID_ARGUMENT &&
		      v90_phase3_sd_next(&sequence, NULL) ==
			      V90_PHASE3_SD_INVALID_ARGUMENT &&
		      v90_phase3_sd_next(&sequence, &symbol) ==
			      V90_PHASE3_SD_INVALID_STATE &&
		      memcmp(&sequence, &saved, sizeof(sequence)) == 0 &&
		      memcmp(&symbol, &saved_symbol, sizeof(symbol)) == 0 &&
		      v90_phase3_sd_remaining(&sequence) == 0 &&
		      v90_phase3_sd_remaining(NULL) == 0,
	      "invalid Sd generation was not transactional");
	CHECK(strcmp(v90_phase3_sd_result_name(V90_PHASE3_SD_COMPLETE),
		     "complete") == 0 &&
		      strcmp(v90_phase3_sd_result_name(
			     (enum v90_phase3_sd_result)4), "unknown") == 0,
	      "Sd result names are wrong");
	return true;
}

struct test_case {
	const char *name;
	bool (*run)(void);
};

int main(void)
{
	static const struct test_case tests[] = {
		{ "exact PCMU and PCMA sequences", test_exact_sequences },
		{ "training-code boundaries", test_training_code_boundaries },
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
