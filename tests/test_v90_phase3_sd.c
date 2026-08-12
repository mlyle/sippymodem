// SPDX-License-Identifier: BSD-3-Clause

#include "test.h"

#include "g711.h"
#include "v90_info1.h"
#include "v90_phase3_sd.h"
#include "v90_phase3_sd_receiver.h"

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

static bool next_octet(
	struct v90_phase3_sd *sequence, uint8_t *octet)
{
	struct v90_pcm_symbol symbol;

	if (v90_phase3_sd_next(sequence, &symbol) != V90_PHASE3_SD_OK)
		return false;
	*octet = symbol.octet;
	return true;
}

static bool check_receiver(
	enum g711_law law, uint8_t training_ucode, uint8_t starting_phase)
{
	struct v90_phase3_sd_receiver receiver;
	struct v90_phase3_sd sd;
	struct v90_phase3_sd sd_bar;
	enum v90_phase3_sd_receiver_result result;
	uint8_t octet;

	CHECK(v90_phase3_sd_receiver_init(
		      &receiver, law, training_ucode) ==
		      V90_PHASE3_SD_RECEIVER_OK &&
		      v90_phase3_sd_init(
			      &sd, law, training_ucode,
			      V90_PHASE3_SD) == V90_PHASE3_SD_OK &&
		      v90_phase3_sd_init(
			      &sd_bar, law, training_ucode,
			      V90_PHASE3_SD_BAR) == V90_PHASE3_SD_OK,
	      "receiver initialization failed for law %u code %u",
	      (unsigned int)law, training_ucode);
	for (uint8_t phase = 0; phase < starting_phase; ++phase)
		CHECK(next_octet(&sd, &octet),
		      "could not skip to Sd phase %u", starting_phase);

	for (size_t index = starting_phase;
	     index < V90_PHASE3_SD_SYMBOLS; ++index) {
		CHECK(next_octet(&sd, &octet),
		      "could not generate Sd symbol %zu", index);
		result = v90_phase3_sd_receiver_receive(&receiver, octet);
		CHECK(result == (index == starting_phase +
				       V90_PHASE3_SD_ACQUISITION_SYMBOLS - 1u ?
				       V90_PHASE3_SD_RECEIVER_SD_DETECTED :
				       V90_PHASE3_SD_RECEIVER_OK),
		      "receiver returned %s at Sd symbol %zu",
		      v90_phase3_sd_receiver_result_name(result), index);
	}
	for (size_t index = 0; index < V90_PHASE3_SD_BAR_SYMBOLS; ++index) {
		CHECK(next_octet(&sd_bar, &octet),
		      "could not generate S-bar symbol %zu", index);
		result = v90_phase3_sd_receiver_receive(&receiver, octet);
		if (index == V90_PHASE3_SD_BAR_QUALIFY_SYMBOLS - 1u)
			CHECK(result ==
				      V90_PHASE3_SD_RECEIVER_SD_BAR_DETECTED,
			      "receiver did not qualify S-bar at symbol %zu",
			      index);
		else if (index == V90_PHASE3_SD_BAR_SYMBOLS - 1u)
			CHECK(result ==
				      V90_PHASE3_SD_RECEIVER_SEQUENCE_COMPLETE,
			      "receiver did not complete S-bar at symbol %zu",
			      index);
		else
			CHECK(result == V90_PHASE3_SD_RECEIVER_OK,
			      "receiver returned %s at S-bar symbol %zu",
			      v90_phase3_sd_receiver_result_name(result), index);
	}
	CHECK(v90_phase3_sd_receiver_is_complete(&receiver),
	      "receiver did not retain completion");
	{
		struct v90_phase3_sd_receiver saved = receiver;

		CHECK(v90_phase3_sd_receiver_receive(&receiver, 0xa5) ==
			      V90_PHASE3_SD_RECEIVER_SEQUENCE_COMPLETE &&
			      memcmp(&receiver, &saved, sizeof(receiver)) == 0,
		      "completed receiver consumed another character");
	}
	return true;
}

static bool test_receiver_exact_sequences(void)
{
	static const uint8_t training_ucodes[] = {
		V90_INFO1A_MIN_TRAINING_UCODE,
		112,
		V90_INFO1A_MAX_TRAINING_UCODE,
	};

	for (enum g711_law law = G711_LAW_PCMU;
	     law <= G711_LAW_PCMA; ++law) {
		for (size_t index = 0;
		     index < ARRAY_LENGTH(training_ucodes); ++index) {
			for (uint8_t phase = 0;
			     phase < V90_PHASE3_SD_PATTERN_SYMBOLS; ++phase) {
				CHECK(check_receiver(
					      law, training_ucodes[index], phase),
				      "receiver failed for law %u code %u phase %u",
				      (unsigned int)law,
				      training_ucodes[index], phase);
			}
		}
	}
	return true;
}

static bool test_receiver_acquisition(void)
{
	struct v90_phase3_sd_receiver receiver;
	struct v90_phase3_sd sd;
	uint32_t noise = UINT32_C(0x36a9f17b);
	uint8_t octet;

	CHECK(v90_phase3_sd_receiver_init(
		      &receiver, G711_LAW_PCMU, 91) ==
		      V90_PHASE3_SD_RECEIVER_OK,
	      "could not initialize receiver for noise test");
	for (size_t index = 0; index < 256; ++index) {
		noise = noise * UINT32_C(1664525) + UINT32_C(1013904223);
		CHECK(v90_phase3_sd_receiver_receive(
			      &receiver, (uint8_t)(noise >> 24)) ==
			      V90_PHASE3_SD_RECEIVER_OK,
		      "random input falsely acquired Sd at symbol %zu", index);
	}
	CHECK(receiver.stage == V90_PHASE3_SD_RECEIVER_SEARCH &&
		      v90_phase3_sd_init(
			      &sd, G711_LAW_PCMU, 91,
			      V90_PHASE3_SD) == V90_PHASE3_SD_OK,
	      "noise search damaged receiver state");
	for (size_t index = 0;
	     index < V90_PHASE3_SD_ACQUISITION_SYMBOLS; ++index) {
		enum v90_phase3_sd_receiver_result result;

		CHECK(next_octet(&sd, &octet),
		      "could not generate acquisition symbol %zu", index);
		if (index == 3 || index == 14)
			octet ^= UINT8_C(0x01);
		result = v90_phase3_sd_receiver_receive(&receiver, octet);
		CHECK(result == (index ==
				       V90_PHASE3_SD_ACQUISITION_SYMBOLS - 1u ?
				       V90_PHASE3_SD_RECEIVER_SD_DETECTED :
				       V90_PHASE3_SD_RECEIVER_OK),
		      "two-error acquisition returned %s at symbol %zu",
		      v90_phase3_sd_receiver_result_name(result), index);
	}
	return true;
}

static bool test_receiver_sbar_error_budget(void)
{
	struct v90_phase3_sd_receiver receiver;
	struct v90_phase3_sd sd;
	struct v90_phase3_sd sd_bar;
	uint8_t octet;

	CHECK(v90_phase3_sd_receiver_init(
		      &receiver, G711_LAW_PCMA, 91) ==
		      V90_PHASE3_SD_RECEIVER_OK &&
		      v90_phase3_sd_init(
			      &sd, G711_LAW_PCMA, 91,
			      V90_PHASE3_SD) == V90_PHASE3_SD_OK &&
		      v90_phase3_sd_init(
			      &sd_bar, G711_LAW_PCMA, 91,
			      V90_PHASE3_SD_BAR) == V90_PHASE3_SD_OK,
	      "could not initialize S-bar error test");
	for (size_t index = 0; index < V90_PHASE3_SD_SYMBOLS; ++index) {
		CHECK(next_octet(&sd, &octet), "could not generate Sd");
		(void)v90_phase3_sd_receiver_receive(&receiver, octet);
	}
	for (size_t index = 0; index < V90_PHASE3_SD_BAR_SYMBOLS; ++index) {
		enum v90_phase3_sd_receiver_result result;

		CHECK(next_octet(&sd_bar, &octet), "could not generate S-bar");
		if (index == 20 || index == 30)
			octet ^= UINT8_C(0x01);
		if (index == 40) {
			struct v90_phase3_sd_receiver saved = receiver;
			uint8_t corrupt = octet ^ UINT8_C(0x01);

			CHECK(v90_phase3_sd_receiver_receive(
				      &receiver, corrupt) ==
				      V90_PHASE3_SD_RECEIVER_INVALID_SEQUENCE &&
				      memcmp(&receiver, &saved,
					     sizeof(receiver)) == 0,
			      "third S-bar error was not transactional");
		}
		result = v90_phase3_sd_receiver_receive(&receiver, octet);
		CHECK(result == (index == V90_PHASE3_SD_BAR_SYMBOLS - 1u ?
				       V90_PHASE3_SD_RECEIVER_SEQUENCE_COMPLETE :
				       index ==
					       V90_PHASE3_SD_BAR_QUALIFY_SYMBOLS - 1u ?
				       V90_PHASE3_SD_RECEIVER_SD_BAR_DETECTED :
				       V90_PHASE3_SD_RECEIVER_OK),
		      "S-bar error test returned %s at symbol %zu",
		      v90_phase3_sd_receiver_result_name(result), index);
	}
	CHECK(receiver.bar_errors == V90_PHASE3_SD_BAR_MAX_ERRORS &&
		      v90_phase3_sd_receiver_is_complete(&receiver),
	      "S-bar receiver did not retain its error budget");
	return true;
}

static bool test_receiver_validation(void)
{
	struct v90_phase3_sd_receiver receiver;
	struct v90_phase3_sd_receiver saved;

	memset(&receiver, 0xa5, sizeof(receiver));
	saved = receiver;
	CHECK(v90_phase3_sd_receiver_init(
		      NULL, G711_LAW_PCMU, 91) ==
		      V90_PHASE3_SD_RECEIVER_INVALID_ARGUMENT &&
		      v90_phase3_sd_receiver_init(
			      &receiver, (enum g711_law)2, 91) ==
			      V90_PHASE3_SD_RECEIVER_INVALID_ARGUMENT &&
		      v90_phase3_sd_receiver_init(
			      &receiver, G711_LAW_PCMU,
			      V90_INFO1A_MIN_TRAINING_UCODE - 1u) ==
			      V90_PHASE3_SD_RECEIVER_INVALID_ARGUMENT &&
		      v90_phase3_sd_receiver_init(
			      &receiver, G711_LAW_PCMU,
			      V90_INFO1A_MAX_TRAINING_UCODE + 1u) ==
			      V90_PHASE3_SD_RECEIVER_INVALID_ARGUMENT &&
		      memcmp(&receiver, &saved, sizeof(receiver)) == 0,
	      "invalid receiver initialization changed state");
	CHECK(v90_phase3_sd_receiver_receive(NULL, 0) ==
		      V90_PHASE3_SD_RECEIVER_INVALID_ARGUMENT &&
		      v90_phase3_sd_receiver_receive(&receiver, 0) ==
			      V90_PHASE3_SD_RECEIVER_INVALID_STATE &&
		      memcmp(&receiver, &saved, sizeof(receiver)) == 0 &&
		      !v90_phase3_sd_receiver_is_valid(&receiver) &&
		      !v90_phase3_sd_receiver_is_complete(&receiver) &&
		      !v90_phase3_sd_receiver_is_valid(NULL),
	      "invalid receiver state was not rejected transactionally");
	CHECK(strcmp(v90_phase3_sd_receiver_result_name(
			     V90_PHASE3_SD_RECEIVER_SD_DETECTED),
		     "Sd detected") == 0 &&
		      strcmp(v90_phase3_sd_receiver_result_name(
			     (enum v90_phase3_sd_receiver_result)7),
			     "unknown") == 0,
	      "receiver result names are wrong");
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
		{ "receiver exact sequences", test_receiver_exact_sequences },
		{ "receiver acquisition", test_receiver_acquisition },
		{ "receiver S-bar error budget",
		  test_receiver_sbar_error_budget },
		{ "receiver validation", test_receiver_validation },
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
