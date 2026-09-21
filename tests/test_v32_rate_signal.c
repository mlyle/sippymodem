// SPDX-License-Identifier: BSD-3-Clause

#include <rate_signal.h>

#include "test.h"

#include <string.h>

static bool offers_are_equal(
	const struct v32_rate_offer *left,
	const struct v32_rate_offer *right)
{
	return left->rates == right->rates &&
	       left->trellis_at_highest_rate == right->trellis_at_highest_rate;
}

static bool test_offer_helpers(void)
{
	static const uint8_t rates[] = {
		V32_RATE_CAPABILITY_4800, V32_RATE_CAPABILITY_7200,
		V32_RATE_CAPABILITY_9600, V32_RATE_CAPABILITY_12000,
		V32_RATE_CAPABILITY_14400,
	};

	for (unsigned int mask = 0; mask <= UINT8_MAX; ++mask) {
		uint8_t expected = 0;

		for (size_t index = 0; index < ARRAY_LENGTH(rates); ++index)
			if ((mask & rates[index]) != 0)
				expected = rates[index];
		CHECK(v32_rate_highest((uint8_t)mask) == expected,
		      "highest capability changed for mask %u", mask);
		for (unsigned int other = 0; other <= UINT8_MAX; ++other) {
			for (unsigned int flags = 0; flags < 4u; ++flags) {
				struct v32_rate_offer left = {
					(uint8_t)mask, (flags & 1u) != 0,
				};
				struct v32_rate_offer right = {
					(uint8_t)other, (flags & 2u) != 0,
				};

				CHECK(v32_rate_offers_match(&left, &right) ==
					      offers_are_equal(&left, &right),
				      "rate-offer equality changed");
			}
		}
	}
	return true;
}

static bool test_retained_offer(void)
{
	struct v32_rate_signal_detector detector = { 0 };
	struct v32_rate_offer offer = { 0 };
	const struct v32_rate_offer empty = offer;

	CHECK(!v32_rate_signal_detector_offer(NULL, &offer) &&
		      !v32_rate_signal_detector_offer(&detector, NULL),
	      "retained offer accepted missing storage");
	for (unsigned int word = 0; word <= UINT16_MAX; ++word) {
		struct v32_rate_offer decoded;
		enum v32_rate_signal_type type;
		bool is_rate = v32_rate_signal_decode(
			(uint16_t)word, &type, &decoded) == V32_RATE_SIGNAL_OK &&
			type == V32_RATE_SIGNAL;

		detector.locked_rate_word = (uint16_t)word;
		detector.aligned = false;
		offer = empty;
		CHECK(!v32_rate_signal_detector_offer(&detector, &offer) &&
			      offers_are_equal(&offer, &empty),
		      "unaligned detector exposed an offer");
		detector.aligned = true;
		const struct v32_rate_signal_detector saved = detector;
		CHECK(v32_rate_signal_detector_offer(&detector, &offer) == is_rate &&
			      (!is_rate || offers_are_equal(&offer, &decoded)) &&
			      memcmp(&detector, &saved, sizeof(detector)) == 0,
		      "retained rate query changed word 0x%04x or detector", word);
	}
	return true;
}

static bool test_normative_words(void)
{
	static const struct {
		struct v32_rate_offer offer;
		uint16_t rate_word;
		uint16_t end_word;
	} fixtures[] = {
		{ { V32_RATE_CAPABILITY_4800, false }, 0x0511,
		  0xf511 },
		{ { V32_RATE_CAPABILITY_7200, true }, 0x09d1,
		  0xf9d1 },
		{ { V32_RATE_CAPABILITY_9600, false }, 0x0311,
		  0xf311 },
		{ { V32_RATE_CAPABILITY_9600, true }, 0x0391,
		  0xf391 },
		{ { V32_RATE_CAPABILITY_12000, true }, 0x09b1,
		  0xf9b1 },
		{ { V32_RATE_CAPABILITY_14400, true }, 0x0999,
		  0xf999 },
	};

	for (size_t index = 0; index < ARRAY_LENGTH(fixtures); ++index) {
		uint16_t word = 0;

		CHECK(v32_rate_signal_encode(
			      V32_RATE_SIGNAL,
			      &fixtures[index].offer, &word) ==
			      V32_RATE_SIGNAL_OK &&
			      word == fixtures[index].rate_word,
		      "V.32 rate word %zu changed to 0x%04x", index, word);
		CHECK(v32_rate_signal_encode(
			      V32_END_SIGNAL,
			      &fixtures[index].offer, &word) ==
			      V32_RATE_SIGNAL_OK &&
			      word == fixtures[index].end_word,
		      "V.32 end word %zu changed to 0x%04x", index, word);
	}
	return true;
}

static bool test_capability_round_trip(void)
{
	struct v32_rate_offer source = {
		.rates = V32_RATE_CAPABILITY_4800 |
			 V32_RATE_CAPABILITY_7200 |
			 V32_RATE_CAPABILITY_9600 |
			 V32_RATE_CAPABILITY_12000 |
			 V32_RATE_CAPABILITY_14400,
		.trellis_at_highest_rate = true,
	};
	struct v32_rate_offer decoded = { 0 };
	enum v32_rate_signal_type type = V32_END_SIGNAL;
	uint16_t word = 0;

	CHECK(v32_rate_signal_encode(
		      V32_RATE_SIGNAL, &source, &word) ==
		      V32_RATE_SIGNAL_OK &&
		      word == 0x0ff9 &&
		      v32_rate_signal_decode(word, &type, &decoded) ==
			      V32_RATE_SIGNAL_OK &&
		      type == V32_RATE_SIGNAL &&
		      offers_are_equal(&source, &decoded),
	      "V.32 capabilities did not survive a rate-word round trip");
	CHECK(v32_rate_signal_decode(0xf511, &type, &decoded) ==
		      V32_RATE_SIGNAL_OK &&
		      type == V32_END_SIGNAL &&
		      decoded.rates == V32_RATE_CAPABILITY_4800 &&
		      !decoded.trellis_at_highest_rate,
	      "V.32 decoder rejected a compatible 4800 end word");
	return true;
}

static bool test_transmission_order(void)
{
	static const uint8_t rate_dibits[] = { 0, 0, 1, 1, 0, 1, 0, 1 };
	static const uint8_t end_dibits[] = { 3, 3, 1, 1, 0, 1, 0, 1 };

	for (size_t index = 0; index < ARRAY_LENGTH(rate_dibits); ++index) {
		uint8_t dibit = UINT8_MAX;

		CHECK(v32_rate_signal_dibit(
			      0x0511, (unsigned int)index, &dibit) ==
			      V32_RATE_SIGNAL_OK &&
			      dibit == rate_dibits[index],
		      "V.32 rate word changed dibit %zu", index);
		CHECK(v32_rate_signal_dibit(
			      0xf511, (unsigned int)index, &dibit) ==
			      V32_RATE_SIGNAL_OK &&
			      dibit == end_dibits[index],
		      "V.32 end word changed dibit %zu", index);
	}
	return true;
}

static bool observe_word(
	struct v32_rate_signal_detector *detector, uint16_t word,
	enum v32_rate_signal_event *event,
	struct v32_rate_offer *offer)
{
	for (unsigned int index = 0;
	     index < V32_RATE_SIGNAL_DIBITS; ++index) {
		uint8_t dibit;

		CHECK(v32_rate_signal_dibit(word, index, &dibit) ==
			      V32_RATE_SIGNAL_OK &&
			      v32_rate_signal_detector_observe(
				      detector, dibit, event, offer) ==
				      V32_RATE_SIGNAL_OK,
		      "V.32 detector could not consume word 0x%04x", word);
		CHECK(index + 1 == V32_RATE_SIGNAL_DIBITS ||
			      *event == V32_RATE_SIGNAL_NO_EVENT,
		      "V.32 detector reported word 0x%04x before its boundary",
		      word);
	}
	return true;
}

static bool test_ignored_receive_extensions(void)
{
	const uint8_t all_rates =
		V32_RATE_CAPABILITY_4800 |
		V32_RATE_CAPABILITY_7200 |
		V32_RATE_CAPABILITY_9600 |
		V32_RATE_CAPABILITY_12000 |
		V32_RATE_CAPABILITY_14400;
	struct v32_rate_signal_detector detector;
	struct v32_rate_offer offer = { 0 };
	enum v32_rate_signal_type type;
	enum v32_rate_signal_event event;

	CHECK(v32_rate_signal_decode(0x0fff, &type, &offer) ==
		      V32_RATE_SIGNAL_OK &&
		      type == V32_RATE_SIGNAL &&
		      offer.rates == all_rates && offer.trellis_at_highest_rate,
	      "V.32 receiver rejected ignored rate-word extension bits");
	CHECK(v32_rate_signal_decode(0x0511, &type, &offer) ==
		      V32_RATE_SIGNAL_OK &&
		      type == V32_RATE_SIGNAL &&
		      offer.rates == V32_RATE_CAPABILITY_4800 &&
		      !offer.trellis_at_highest_rate,
	      "V.32 receiver rejected a base-mode rate word without B4");
	{
		struct v32_rate_offer saved = offer;
		enum v32_rate_signal_type saved_type = type;

		CHECK(v32_rate_signal_decode(0xfd17, &type, &offer) ==
			      V32_RATE_SIGNAL_INVALID_WORD &&
			      type == saved_type && offers_are_equal(&offer, &saved),
		      "V.32 receiver ignored reserved bits in sequence E");
	}
	CHECK(v32_rate_signal_detector_init(&detector) ==
		      V32_RATE_SIGNAL_OK &&
		      observe_word(&detector, 0x0fff, &event, &offer) &&
		      event == V32_RATE_SIGNAL_NO_EVENT &&
		      observe_word(&detector, 0x0fff, &event, &offer) &&
		      event == V32_RATE_SIGNAL_DETECTED &&
		      offer.rates == all_rates,
	      "V.32 detector did not lock through ignored extension bits");
	return true;
}

static bool test_repeated_rate_detection(void)
{
	struct v32_rate_signal_detector detector;
	struct v32_rate_offer offer = { 0 };
	enum v32_rate_signal_event event =
		V32_END_SIGNAL_DETECTED;

	CHECK(v32_rate_signal_detector_init(&detector) ==
		      V32_RATE_SIGNAL_OK &&
		      v32_rate_signal_detector_observe(
			      &detector, 2, &event, &offer) ==
			      V32_RATE_SIGNAL_OK &&
		      event == V32_RATE_SIGNAL_NO_EVENT,
	      "V.32 detector did not accept an unaligned preamble");
	CHECK(observe_word(&detector, 0x0511, &event, &offer) &&
		      event == V32_RATE_SIGNAL_NO_EVENT,
	      "V.32 detector accepted only one rate word");
	CHECK(observe_word(&detector, 0x0511, &event, &offer) &&
		      event == V32_RATE_SIGNAL_DETECTED &&
		      offer.rates == V32_RATE_CAPABILITY_4800 &&
		      !offer.trellis_at_highest_rate && detector.aligned,
	      "V.32 detector did not align to repeated rate words");
	return true;
}

static bool test_aligned_end_detection(void)
{
	struct v32_rate_signal_detector detector;
	struct v32_rate_offer offer = { 0 };
	enum v32_rate_signal_event event =
		V32_RATE_SIGNAL_NO_EVENT;

	CHECK(v32_rate_signal_detector_init(&detector) ==
		      V32_RATE_SIGNAL_OK &&
		      observe_word(&detector, 0x0511, &event, &offer) &&
		      event == V32_RATE_SIGNAL_NO_EVENT &&
		      observe_word(&detector, 0x0511, &event, &offer) &&
		      event == V32_RATE_SIGNAL_DETECTED,
	      "V.32 detector did not acquire the peer rate sequence");
	CHECK(observe_word(&detector, 0x0511, &event, &offer) &&
		      event == V32_RATE_SIGNAL_DETECTED,
	      "V.32 detector lost an aligned repeated word");
	CHECK(observe_word(&detector, 0xf511, &event, &offer) &&
		      event == V32_END_SIGNAL_DETECTED &&
		      (offer.rates & V32_RATE_CAPABILITY_4800) != 0 &&
		      v32_rate_signal_detector_is_valid(&detector) &&
		      !detector.aligned,
	      "V.32 detector did not recognize the aligned end word");
	return true;
}

static bool test_detector_reacquisition(void)
{
	struct v32_rate_signal_detector detector;
	struct v32_rate_offer offer = { 0 };
	enum v32_rate_signal_event event =
		V32_RATE_SIGNAL_NO_EVENT;

	CHECK(v32_rate_signal_detector_init(&detector) ==
		      V32_RATE_SIGNAL_OK &&
		      observe_word(&detector, 0x0511, &event, &offer) &&
		      observe_word(&detector, 0x0511, &event, &offer) &&
		      event == V32_RATE_SIGNAL_DETECTED,
	      "V.32 reacquisition fixture did not lock");
	CHECK(observe_word(&detector, 0x0551, &event, &offer) &&
		      event == V32_RATE_SIGNAL_NO_EVENT &&
		      !detector.aligned &&
		      observe_word(&detector, 0x0311, &event, &offer) &&
		      event == V32_RATE_SIGNAL_NO_EVENT &&
		      observe_word(&detector, 0x0311, &event, &offer) &&
		      event == V32_RATE_SIGNAL_DETECTED &&
		      offer.rates == V32_RATE_CAPABILITY_9600,
	      "V.32 detector did not reacquire after a malformed word");
	return true;
}

static bool test_detector_validation(void)
{
	struct v32_rate_signal_detector detector;
	struct v32_rate_signal_detector saved;
	struct v32_rate_offer offer = {
		.rates = UINT8_MAX,
		.trellis_at_highest_rate = true,
	};
	struct v32_rate_offer saved_offer = offer;
	enum v32_rate_signal_event event =
		V32_END_SIGNAL_DETECTED;

	CHECK(v32_rate_signal_detector_init(&detector) ==
		      V32_RATE_SIGNAL_OK,
	      "V.32 validation detector did not initialize");
	saved = detector;
	CHECK(v32_rate_signal_detector_observe(
		      &detector, 4, &event, &offer) ==
		      V32_RATE_SIGNAL_INVALID_ARGUMENT &&
		      memcmp(&detector, &saved, sizeof(detector)) == 0 &&
		      event == V32_END_SIGNAL_DETECTED &&
		      offers_are_equal(&offer, &saved_offer) &&
		      v32_rate_signal_detector_init(NULL) ==
			      V32_RATE_SIGNAL_INVALID_ARGUMENT,
	      "V.32 detector accepted invalid input or changed outputs");
	detector.dibits_since_boundary =
		V32_RATE_SIGNAL_DIBITS;
	CHECK(!v32_rate_signal_detector_is_valid(&detector) &&
		      v32_rate_signal_detector_observe(
			      &detector, 0, &event, &offer) ==
			      V32_RATE_SIGNAL_INVALID_ARGUMENT,
	      "V.32 detector accepted corrupt alignment state");
	return true;
}

static bool test_validation(void)
{
	struct v32_rate_offer no_rates = { 0 };
	struct v32_rate_offer multiple_rates = {
		.rates = V32_RATE_CAPABILITY_4800 |
			 V32_RATE_CAPABILITY_9600,
	};
	struct v32_rate_offer multiple_trellis_rates = {
		.rates = V32_RATE_CAPABILITY_4800 |
			 V32_RATE_CAPABILITY_12000,
		.trellis_at_highest_rate = true,
	};
	struct v32_rate_offer uncoded_12000 = {
		.rates = V32_RATE_CAPABILITY_12000,
	};
	struct v32_rate_offer decoded = {
		.rates = UINT8_MAX,
		.trellis_at_highest_rate = true,
	};
	struct v32_rate_offer saved = decoded;
	enum v32_rate_signal_type type = V32_END_SIGNAL;
	uint16_t word = UINT16_MAX;
	uint8_t dibit = UINT8_MAX;

	CHECK(v32_rate_signal_encode(
		      V32_RATE_SIGNAL, &no_rates, &word) ==
		      V32_RATE_SIGNAL_INVALID_OFFER &&
		      v32_rate_signal_encode(
			      V32_END_SIGNAL, &multiple_rates,
			      &word) ==
			      V32_RATE_SIGNAL_INVALID_OFFER &&
		      v32_rate_signal_encode(
			      V32_END_SIGNAL,
			      &multiple_trellis_rates, &word) ==
			      V32_RATE_SIGNAL_INVALID_OFFER &&
		      v32_rate_signal_encode(
			      V32_RATE_SIGNAL, &uncoded_12000,
			      &word) ==
			      V32_RATE_SIGNAL_INVALID_OFFER &&
		      v32_rate_signal_encode(
			      (enum v32_rate_signal_type)2,
			      &multiple_rates, &word) ==
			      V32_RATE_SIGNAL_INVALID_ARGUMENT &&
		      word == UINT16_MAX,
	      "V.32 encoder accepted an invalid offer or changed output");
	CHECK(v32_rate_signal_decode(
		      0x0551, &type, &decoded) ==
		      V32_RATE_SIGNAL_INVALID_WORD &&
		      type == V32_END_SIGNAL &&
		      offers_are_equal(&decoded, &saved) &&
		      v32_rate_signal_decode(
			      0x0111, &type, &decoded) ==
			      V32_RATE_SIGNAL_INVALID_WORD &&
		      offers_are_equal(&decoded, &saved),
	      "V.32 decoder accepted reserved bits or an empty offer");
	CHECK(v32_rate_signal_dibit(
		      0x0511, V32_RATE_SIGNAL_DIBITS, &dibit) ==
		      V32_RATE_SIGNAL_INVALID_ARGUMENT &&
		      dibit == UINT8_MAX,
	      "V.32 rate-word validation changed output");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "shared rate-offer helpers", test_offer_helpers },
		{ "aligned retained offer", test_retained_offer },
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
