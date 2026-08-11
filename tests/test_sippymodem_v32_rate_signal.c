// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_v32_rate_signal.h>

#include "test.h"

#include <string.h>

static bool offers_are_equal(
	const struct sippymodem_v32_rate_offer *left,
	const struct sippymodem_v32_rate_offer *right)
{
	return left->rates == right->rates &&
	       left->trellis_at_highest_rate == right->trellis_at_highest_rate;
}

static bool test_normative_words(void)
{
	static const struct {
		struct sippymodem_v32_rate_offer offer;
		uint16_t rate_word;
		uint16_t end_word;
	} fixtures[] = {
		{ { SIPPYMODEM_V32_RATE_CAPABILITY_4800, false }, 0x0d11,
		  0xfd11 },
		{ { SIPPYMODEM_V32_RATE_CAPABILITY_7200, true }, 0x09d1,
		  0xf9d1 },
		{ { SIPPYMODEM_V32_RATE_CAPABILITY_9600, false }, 0x0b11,
		  0xfb11 },
		{ { SIPPYMODEM_V32_RATE_CAPABILITY_9600, true }, 0x0b91,
		  0xfb91 },
		{ { SIPPYMODEM_V32_RATE_CAPABILITY_12000, true }, 0x09b1,
		  0xf9b1 },
		{ { SIPPYMODEM_V32_RATE_CAPABILITY_14400, true }, 0x0999,
		  0xf999 },
	};

	for (size_t index = 0; index < ARRAY_LENGTH(fixtures); ++index) {
		uint16_t word = 0;

		CHECK(sippymodem_v32_rate_signal_encode(
			      SIPPYMODEM_V32_RATE_SIGNAL,
			      &fixtures[index].offer, &word) ==
			      SIPPYMODEM_V32_RATE_SIGNAL_OK &&
			      word == fixtures[index].rate_word,
		      "V.32 rate word %zu changed to 0x%04x", index, word);
		CHECK(sippymodem_v32_rate_signal_encode(
			      SIPPYMODEM_V32_END_SIGNAL,
			      &fixtures[index].offer, &word) ==
			      SIPPYMODEM_V32_RATE_SIGNAL_OK &&
			      word == fixtures[index].end_word,
		      "V.32 end word %zu changed to 0x%04x", index, word);
	}
	return true;
}

static bool test_capability_round_trip(void)
{
	struct sippymodem_v32_rate_offer source = {
		.rates = SIPPYMODEM_V32_RATE_CAPABILITY_4800 |
			 SIPPYMODEM_V32_RATE_CAPABILITY_7200 |
			 SIPPYMODEM_V32_RATE_CAPABILITY_9600 |
			 SIPPYMODEM_V32_RATE_CAPABILITY_12000 |
			 SIPPYMODEM_V32_RATE_CAPABILITY_14400,
		.trellis_at_highest_rate = true,
	};
	struct sippymodem_v32_rate_offer decoded = { 0 };
	enum sippymodem_v32_rate_signal_type type = SIPPYMODEM_V32_END_SIGNAL;
	uint16_t word = 0;

	CHECK(sippymodem_v32_rate_signal_encode(
		      SIPPYMODEM_V32_RATE_SIGNAL, &source, &word) ==
		      SIPPYMODEM_V32_RATE_SIGNAL_OK &&
		      word == 0x0ff9 &&
		      sippymodem_v32_rate_signal_decode(word, &type, &decoded) ==
			      SIPPYMODEM_V32_RATE_SIGNAL_OK &&
		      type == SIPPYMODEM_V32_RATE_SIGNAL &&
		      offers_are_equal(&source, &decoded),
	      "V.32 capabilities did not survive a rate-word round trip");
	CHECK(sippymodem_v32_rate_signal_decode(0xfd11, &type, &decoded) ==
		      SIPPYMODEM_V32_RATE_SIGNAL_OK &&
		      type == SIPPYMODEM_V32_END_SIGNAL &&
		      decoded.rates == SIPPYMODEM_V32_RATE_CAPABILITY_4800 &&
		      !decoded.trellis_at_highest_rate,
	      "V.32 decoder rejected a compatible 4800 end word");
	return true;
}

static bool test_transmission_order(void)
{
	static const uint8_t rate_dibits[] = { 0, 0, 3, 1, 0, 1, 0, 1 };
	static const uint8_t end_dibits[] = { 3, 3, 3, 1, 0, 1, 0, 1 };

	for (size_t index = 0; index < ARRAY_LENGTH(rate_dibits); ++index) {
		uint8_t dibit = UINT8_MAX;

		CHECK(sippymodem_v32_rate_signal_dibit(
			      0x0d11, (unsigned int)index, &dibit) ==
			      SIPPYMODEM_V32_RATE_SIGNAL_OK &&
			      dibit == rate_dibits[index],
		      "V.32 rate word changed dibit %zu", index);
		CHECK(sippymodem_v32_rate_signal_dibit(
			      0xfd11, (unsigned int)index, &dibit) ==
			      SIPPYMODEM_V32_RATE_SIGNAL_OK &&
			      dibit == end_dibits[index],
		      "V.32 end word changed dibit %zu", index);
	}
	return true;
}

static bool observe_word(
	struct sippymodem_v32_rate_signal_detector *detector, uint16_t word,
	enum sippymodem_v32_rate_signal_event *event,
	struct sippymodem_v32_rate_offer *offer)
{
	for (unsigned int index = 0;
	     index < SIPPYMODEM_V32_RATE_SIGNAL_DIBITS; ++index) {
		uint8_t dibit;

		CHECK(sippymodem_v32_rate_signal_dibit(word, index, &dibit) ==
			      SIPPYMODEM_V32_RATE_SIGNAL_OK &&
			      sippymodem_v32_rate_signal_detector_observe(
				      detector, dibit, event, offer) ==
				      SIPPYMODEM_V32_RATE_SIGNAL_OK,
		      "V.32 detector could not consume word 0x%04x", word);
		CHECK(index + 1 == SIPPYMODEM_V32_RATE_SIGNAL_DIBITS ||
			      *event == SIPPYMODEM_V32_RATE_SIGNAL_NO_EVENT,
		      "V.32 detector reported word 0x%04x before its boundary",
		      word);
	}
	return true;
}

static bool test_ignored_receive_extensions(void)
{
	const uint8_t all_rates =
		SIPPYMODEM_V32_RATE_CAPABILITY_4800 |
		SIPPYMODEM_V32_RATE_CAPABILITY_7200 |
		SIPPYMODEM_V32_RATE_CAPABILITY_9600 |
		SIPPYMODEM_V32_RATE_CAPABILITY_12000 |
		SIPPYMODEM_V32_RATE_CAPABILITY_14400;
	struct sippymodem_v32_rate_signal_detector detector;
	struct sippymodem_v32_rate_offer offer = { 0 };
	enum sippymodem_v32_rate_signal_type type;
	enum sippymodem_v32_rate_signal_event event;

	CHECK(sippymodem_v32_rate_signal_decode(0x0fff, &type, &offer) ==
		      SIPPYMODEM_V32_RATE_SIGNAL_OK &&
		      type == SIPPYMODEM_V32_RATE_SIGNAL &&
		      offer.rates == all_rates && offer.trellis_at_highest_rate,
	      "V.32 receiver rejected ignored rate-word extension bits");
	CHECK(sippymodem_v32_rate_signal_decode(0x0511, &type, &offer) ==
		      SIPPYMODEM_V32_RATE_SIGNAL_OK &&
		      type == SIPPYMODEM_V32_RATE_SIGNAL &&
		      offer.rates == SIPPYMODEM_V32_RATE_CAPABILITY_4800 &&
		      !offer.trellis_at_highest_rate,
	      "V.32 receiver rejected a base-mode rate word without B4");
	{
		struct sippymodem_v32_rate_offer saved = offer;
		enum sippymodem_v32_rate_signal_type saved_type = type;

		CHECK(sippymodem_v32_rate_signal_decode(0xfd17, &type, &offer) ==
			      SIPPYMODEM_V32_RATE_SIGNAL_INVALID_WORD &&
			      type == saved_type && offers_are_equal(&offer, &saved),
		      "V.32 receiver ignored reserved bits in sequence E");
	}
	CHECK(sippymodem_v32_rate_signal_detector_init(&detector) ==
		      SIPPYMODEM_V32_RATE_SIGNAL_OK &&
		      observe_word(&detector, 0x0fff, &event, &offer) &&
		      event == SIPPYMODEM_V32_RATE_SIGNAL_NO_EVENT &&
		      observe_word(&detector, 0x0fff, &event, &offer) &&
		      event == SIPPYMODEM_V32_RATE_SIGNAL_DETECTED &&
		      offer.rates == all_rates,
	      "V.32 detector did not lock through ignored extension bits");
	return true;
}

static bool test_repeated_rate_detection(void)
{
	struct sippymodem_v32_rate_signal_detector detector;
	struct sippymodem_v32_rate_offer offer = { 0 };
	enum sippymodem_v32_rate_signal_event event =
		SIPPYMODEM_V32_END_SIGNAL_DETECTED;

	CHECK(sippymodem_v32_rate_signal_detector_init(&detector) ==
		      SIPPYMODEM_V32_RATE_SIGNAL_OK &&
		      sippymodem_v32_rate_signal_detector_observe(
			      &detector, 2, &event, &offer) ==
			      SIPPYMODEM_V32_RATE_SIGNAL_OK &&
		      event == SIPPYMODEM_V32_RATE_SIGNAL_NO_EVENT,
	      "V.32 detector did not accept an unaligned preamble");
	CHECK(observe_word(&detector, 0x0d11, &event, &offer) &&
		      event == SIPPYMODEM_V32_RATE_SIGNAL_NO_EVENT,
	      "V.32 detector accepted only one rate word");
	CHECK(observe_word(&detector, 0x0d11, &event, &offer) &&
		      event == SIPPYMODEM_V32_RATE_SIGNAL_DETECTED &&
		      offer.rates == SIPPYMODEM_V32_RATE_CAPABILITY_4800 &&
		      !offer.trellis_at_highest_rate && detector.aligned,
	      "V.32 detector did not align to repeated rate words");
	return true;
}

static bool test_aligned_end_detection(void)
{
	struct sippymodem_v32_rate_signal_detector detector;
	struct sippymodem_v32_rate_offer offer = { 0 };
	enum sippymodem_v32_rate_signal_event event =
		SIPPYMODEM_V32_RATE_SIGNAL_NO_EVENT;

	CHECK(sippymodem_v32_rate_signal_detector_init(&detector) ==
		      SIPPYMODEM_V32_RATE_SIGNAL_OK &&
		      observe_word(&detector, 0x0d11, &event, &offer) &&
		      event == SIPPYMODEM_V32_RATE_SIGNAL_NO_EVENT &&
		      observe_word(&detector, 0x0d11, &event, &offer) &&
		      event == SIPPYMODEM_V32_RATE_SIGNAL_DETECTED,
	      "V.32 detector did not acquire the peer rate sequence");
	CHECK(observe_word(&detector, 0x0d11, &event, &offer) &&
		      event == SIPPYMODEM_V32_RATE_SIGNAL_DETECTED,
	      "V.32 detector lost an aligned repeated word");
	CHECK(observe_word(&detector, 0xfd11, &event, &offer) &&
		      event == SIPPYMODEM_V32_END_SIGNAL_DETECTED &&
		      (offer.rates & SIPPYMODEM_V32_RATE_CAPABILITY_4800) != 0 &&
		      sippymodem_v32_rate_signal_detector_is_valid(&detector) &&
		      !detector.aligned,
	      "V.32 detector did not recognize the aligned end word");
	return true;
}

static bool test_detector_reacquisition(void)
{
	struct sippymodem_v32_rate_signal_detector detector;
	struct sippymodem_v32_rate_offer offer = { 0 };
	enum sippymodem_v32_rate_signal_event event =
		SIPPYMODEM_V32_RATE_SIGNAL_NO_EVENT;

	CHECK(sippymodem_v32_rate_signal_detector_init(&detector) ==
		      SIPPYMODEM_V32_RATE_SIGNAL_OK &&
		      observe_word(&detector, 0x0d11, &event, &offer) &&
		      observe_word(&detector, 0x0d11, &event, &offer) &&
		      event == SIPPYMODEM_V32_RATE_SIGNAL_DETECTED,
	      "V.32 reacquisition fixture did not lock");
	CHECK(observe_word(&detector, 0x0551, &event, &offer) &&
		      event == SIPPYMODEM_V32_RATE_SIGNAL_NO_EVENT &&
		      !detector.aligned &&
		      observe_word(&detector, 0x0b11, &event, &offer) &&
		      event == SIPPYMODEM_V32_RATE_SIGNAL_NO_EVENT &&
		      observe_word(&detector, 0x0b11, &event, &offer) &&
		      event == SIPPYMODEM_V32_RATE_SIGNAL_DETECTED &&
		      offer.rates == SIPPYMODEM_V32_RATE_CAPABILITY_9600,
	      "V.32 detector did not reacquire after a malformed word");
	return true;
}

static bool test_detector_validation(void)
{
	struct sippymodem_v32_rate_signal_detector detector;
	struct sippymodem_v32_rate_signal_detector saved;
	struct sippymodem_v32_rate_offer offer = {
		.rates = UINT8_MAX,
		.trellis_at_highest_rate = true,
	};
	struct sippymodem_v32_rate_offer saved_offer = offer;
	enum sippymodem_v32_rate_signal_event event =
		SIPPYMODEM_V32_END_SIGNAL_DETECTED;

	CHECK(sippymodem_v32_rate_signal_detector_init(&detector) ==
		      SIPPYMODEM_V32_RATE_SIGNAL_OK,
	      "V.32 validation detector did not initialize");
	saved = detector;
	CHECK(sippymodem_v32_rate_signal_detector_observe(
		      &detector, 4, &event, &offer) ==
		      SIPPYMODEM_V32_RATE_SIGNAL_INVALID_ARGUMENT &&
		      memcmp(&detector, &saved, sizeof(detector)) == 0 &&
		      event == SIPPYMODEM_V32_END_SIGNAL_DETECTED &&
		      offers_are_equal(&offer, &saved_offer) &&
		      sippymodem_v32_rate_signal_detector_init(NULL) ==
			      SIPPYMODEM_V32_RATE_SIGNAL_INVALID_ARGUMENT,
	      "V.32 detector accepted invalid input or changed outputs");
	detector.dibits_since_boundary =
		SIPPYMODEM_V32_RATE_SIGNAL_DIBITS;
	CHECK(!sippymodem_v32_rate_signal_detector_is_valid(&detector) &&
		      sippymodem_v32_rate_signal_detector_observe(
			      &detector, 0, &event, &offer) ==
			      SIPPYMODEM_V32_RATE_SIGNAL_INVALID_ARGUMENT,
	      "V.32 detector accepted corrupt alignment state");
	return true;
}

static bool test_validation(void)
{
	struct sippymodem_v32_rate_offer no_rates = { 0 };
	struct sippymodem_v32_rate_offer multiple_rates = {
		.rates = SIPPYMODEM_V32_RATE_CAPABILITY_4800 |
			 SIPPYMODEM_V32_RATE_CAPABILITY_9600,
	};
	struct sippymodem_v32_rate_offer multiple_trellis_rates = {
		.rates = SIPPYMODEM_V32_RATE_CAPABILITY_4800 |
			 SIPPYMODEM_V32_RATE_CAPABILITY_12000,
		.trellis_at_highest_rate = true,
	};
	struct sippymodem_v32_rate_offer uncoded_12000 = {
		.rates = SIPPYMODEM_V32_RATE_CAPABILITY_12000,
	};
	struct sippymodem_v32_rate_offer decoded = {
		.rates = UINT8_MAX,
		.trellis_at_highest_rate = true,
	};
	struct sippymodem_v32_rate_offer saved = decoded;
	enum sippymodem_v32_rate_signal_type type = SIPPYMODEM_V32_END_SIGNAL;
	uint16_t word = UINT16_MAX;
	uint8_t dibit = UINT8_MAX;

	CHECK(sippymodem_v32_rate_signal_encode(
		      SIPPYMODEM_V32_RATE_SIGNAL, &no_rates, &word) ==
		      SIPPYMODEM_V32_RATE_SIGNAL_INVALID_OFFER &&
		      sippymodem_v32_rate_signal_encode(
			      SIPPYMODEM_V32_END_SIGNAL, &multiple_rates,
			      &word) ==
			      SIPPYMODEM_V32_RATE_SIGNAL_INVALID_OFFER &&
		      sippymodem_v32_rate_signal_encode(
			      SIPPYMODEM_V32_END_SIGNAL,
			      &multiple_trellis_rates, &word) ==
			      SIPPYMODEM_V32_RATE_SIGNAL_INVALID_OFFER &&
		      sippymodem_v32_rate_signal_encode(
			      SIPPYMODEM_V32_RATE_SIGNAL, &uncoded_12000,
			      &word) ==
			      SIPPYMODEM_V32_RATE_SIGNAL_INVALID_OFFER &&
		      sippymodem_v32_rate_signal_encode(
			      (enum sippymodem_v32_rate_signal_type)2,
			      &multiple_rates, &word) ==
			      SIPPYMODEM_V32_RATE_SIGNAL_INVALID_ARGUMENT &&
		      word == UINT16_MAX,
	      "V.32 encoder accepted an invalid offer or changed output");
	CHECK(sippymodem_v32_rate_signal_decode(
		      0x0551, &type, &decoded) ==
		      SIPPYMODEM_V32_RATE_SIGNAL_INVALID_WORD &&
		      type == SIPPYMODEM_V32_END_SIGNAL &&
		      offers_are_equal(&decoded, &saved) &&
		      sippymodem_v32_rate_signal_decode(
			      0x0111, &type, &decoded) ==
			      SIPPYMODEM_V32_RATE_SIGNAL_INVALID_WORD &&
		      offers_are_equal(&decoded, &saved),
	      "V.32 decoder accepted reserved bits or an empty offer");
	CHECK(sippymodem_v32_rate_signal_dibit(
		      0x0d11, SIPPYMODEM_V32_RATE_SIGNAL_DIBITS, &dibit) ==
		      SIPPYMODEM_V32_RATE_SIGNAL_INVALID_ARGUMENT &&
		      dibit == UINT8_MAX &&
		      strcmp(sippymodem_v32_rate_signal_result_name(
			     SIPPYMODEM_V32_RATE_SIGNAL_INVALID_WORD),
			     "invalid word") == 0,
	      "V.32 rate-word validation changed output or diagnostics");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "normative words", test_normative_words },
		{ "capability round trip", test_capability_round_trip },
		{ "transmission order", test_transmission_order },
		{ "ignored receive extensions",
		  test_ignored_receive_extensions },
		{ "repeated rate detection", test_repeated_rate_detection },
		{ "aligned end detection", test_aligned_end_detection },
		{ "detector reacquisition", test_detector_reacquisition },
		{ "detector validation", test_detector_validation },
		{ "validation", test_validation },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		if (!tests[index].run()) {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failures++;
		} else {
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		}
	}
	return failures == 0 ? 0 : 1;
}
