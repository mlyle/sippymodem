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
		{ { SIPPYMODEM_V32_RATE_CAPABILITY_2400, false }, 0x0911,
		  0xf911 },
		{ { SIPPYMODEM_V32_RATE_CAPABILITY_4800, false }, 0x0511,
		  0xf511 },
		{ { SIPPYMODEM_V32_RATE_CAPABILITY_9600, true }, 0x0391,
		  0xf391 },
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
		.rates = SIPPYMODEM_V32_RATE_CAPABILITY_2400 |
			 SIPPYMODEM_V32_RATE_CAPABILITY_4800 |
			 SIPPYMODEM_V32_RATE_CAPABILITY_9600,
		.trellis_at_highest_rate = true,
	};
	struct sippymodem_v32_rate_offer decoded = { 0 };
	enum sippymodem_v32_rate_signal_type type = SIPPYMODEM_V32_END_SIGNAL;
	uint16_t word = 0;

	CHECK(sippymodem_v32_rate_signal_encode(
		      SIPPYMODEM_V32_RATE_SIGNAL, &source, &word) ==
		      SIPPYMODEM_V32_RATE_SIGNAL_OK &&
		      word == 0x0f91 &&
		      sippymodem_v32_rate_signal_decode(word, &type, &decoded) ==
			      SIPPYMODEM_V32_RATE_SIGNAL_OK &&
		      type == SIPPYMODEM_V32_RATE_SIGNAL &&
		      offers_are_equal(&source, &decoded),
	      "V.32 capabilities did not survive a rate-word round trip");
	/* Accept a peer that retains its 2400 fallback in a 4800 end word. */
	CHECK(sippymodem_v32_rate_signal_decode(0xfd11, &type, &decoded) ==
		      SIPPYMODEM_V32_RATE_SIGNAL_OK &&
		      type == SIPPYMODEM_V32_END_SIGNAL &&
		      decoded.rates ==
			      (SIPPYMODEM_V32_RATE_CAPABILITY_2400 |
			       SIPPYMODEM_V32_RATE_CAPABILITY_4800) &&
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

		CHECK(sippymodem_v32_rate_signal_dibit(
			      0x0511, (unsigned int)index, &dibit) ==
			      SIPPYMODEM_V32_RATE_SIGNAL_OK &&
			      dibit == rate_dibits[index],
		      "V.32 rate word changed dibit %zu", index);
		CHECK(sippymodem_v32_rate_signal_dibit(
			      0xf511, (unsigned int)index, &dibit) ==
			      SIPPYMODEM_V32_RATE_SIGNAL_OK &&
			      dibit == end_dibits[index],
		      "V.32 end word changed dibit %zu", index);
	}
	return true;
}

static bool test_validation(void)
{
	struct sippymodem_v32_rate_offer no_rates = { 0 };
	struct sippymodem_v32_rate_offer multiple_rates = {
		.rates = SIPPYMODEM_V32_RATE_CAPABILITY_4800 |
			 SIPPYMODEM_V32_RATE_CAPABILITY_9600,
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
		      0x0511, SIPPYMODEM_V32_RATE_SIGNAL_DIBITS, &dibit) ==
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
