// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_v34_framing.h>

#include "test.h"

#include <string.h>

static uint32_t fixture_random(uint32_t *state)
{
	*state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
	return *state;
}

static bool test_polynomial_starts(void)
{
	static const uint8_t expected[][15] = {
		[SIPPYMODEM_V34_POLYNOMIAL_GPC] = {
			3, 3, 3, 3, 3, 3, 3, 3, 3, 0, 0, 1, 3, 3, 3,
		},
		[SIPPYMODEM_V34_POLYNOMIAL_GPA] = {
			3, 3, 2, 0, 0, 3, 3, 2, 0, 0, 3, 2, 1, 3, 3,
		},
	};

	for (size_t polynomial = 0; polynomial < ARRAY_LENGTH(expected);
	     ++polynomial) {
		struct sippymodem_v34_scrambler scrambler;

		CHECK(sippymodem_v34_scrambler_init(
			      &scrambler,
			      (enum sippymodem_v34_scrambler_polynomial)
				      polynomial) == SIPPYMODEM_V34_FRAMING_OK &&
		      scrambler.history == 0 &&
		      scrambler.polynomial ==
			      (enum sippymodem_v34_scrambler_polynomial)
				      polynomial,
		      "V.34 polynomial fixture did not initialize");
		for (size_t index = 0; index < ARRAY_LENGTH(expected[polynomial]);
		     ++index) {
			uint8_t output;

			CHECK(sippymodem_v34_scramble_group(
				      &scrambler, 3, 2, &output) ==
				      SIPPYMODEM_V34_FRAMING_OK &&
			      output == expected[polynomial][index],
			      "V.34 polynomial start changed at %zu/%zu",
			      polynomial, index);
		}
	}
	return true;
}

static bool test_station_role_polynomials(void)
{
	static const struct {
		enum sippymodem_v34_station_role role;
		enum sippymodem_v34_scrambler_polynomial polynomial;
	} cases[] = {
		{ SIPPYMODEM_V34_CALLING_STATION,
		  SIPPYMODEM_V34_POLYNOMIAL_GPC },
		{ SIPPYMODEM_V34_ANSWERING_STATION,
		  SIPPYMODEM_V34_POLYNOMIAL_GPA },
	};
	struct sippymodem_v34_scrambler scrambler;
	struct sippymodem_v34_scrambler before;

	for (size_t index = 0; index < ARRAY_LENGTH(cases); ++index) {
		enum sippymodem_v34_station_role other =
			cases[index].role == SIPPYMODEM_V34_CALLING_STATION ?
				SIPPYMODEM_V34_ANSWERING_STATION :
				SIPPYMODEM_V34_CALLING_STATION;

		CHECK(sippymodem_v34_transmit_scrambler_init(
			      &scrambler, cases[index].role) ==
			      SIPPYMODEM_V34_FRAMING_OK &&
		      scrambler.history == 0 &&
		      scrambler.polynomial == cases[index].polynomial &&
		      sippymodem_v34_transmit_scrambler_is_valid(
			      &scrambler, cases[index].role) &&
		      !sippymodem_v34_transmit_scrambler_is_valid(
			      &scrambler, other),
		      "V.34 station role %zu selected the wrong polynomial",
		      index);
	}
	before = scrambler;
	CHECK(sippymodem_v34_transmit_scrambler_init(
		      NULL, SIPPYMODEM_V34_CALLING_STATION) ==
		      SIPPYMODEM_V34_FRAMING_INVALID_ARGUMENT &&
	      sippymodem_v34_transmit_scrambler_init(
		      &scrambler, (enum sippymodem_v34_station_role)99) ==
		      SIPPYMODEM_V34_FRAMING_INVALID_ARGUMENT &&
	      memcmp(&scrambler, &before, sizeof(scrambler)) == 0 &&
	      !sippymodem_v34_transmit_scrambler_is_valid(
		      NULL, SIPPYMODEM_V34_CALLING_STATION) &&
	      !sippymodem_v34_transmit_scrambler_is_valid(
		      &scrambler, (enum sippymodem_v34_station_role)99),
	      "V.34 station scrambler accepted invalid input");
	return true;
}

static bool test_group_round_trip(void)
{
	uint32_t random = UINT32_C(0x34f2b917);

	for (unsigned int polynomial = 0; polynomial < 2; ++polynomial) {
		struct sippymodem_v34_scrambler scrambler;
		struct sippymodem_v34_scrambler descrambler;

		CHECK(sippymodem_v34_scrambler_init(
			      &scrambler,
			      (enum sippymodem_v34_scrambler_polynomial)
				      polynomial) == SIPPYMODEM_V34_FRAMING_OK &&
		      sippymodem_v34_scrambler_init(
			      &descrambler,
			      (enum sippymodem_v34_scrambler_polynomial)
				      polynomial) == SIPPYMODEM_V34_FRAMING_OK,
		      "V.34 round-trip fixtures did not initialize");
		for (size_t index = 0; index < 2048; ++index) {
			unsigned int bit_count = (unsigned int)(index % 8u) + 1u;
			uint8_t mask = (uint8_t)((1u << bit_count) - 1u);
			uint8_t input = (uint8_t)fixture_random(&random) & mask;
			uint8_t encoded;
			uint8_t output;

			CHECK(sippymodem_v34_scramble_group(
				      &scrambler, input, bit_count, &encoded) ==
				      SIPPYMODEM_V34_FRAMING_OK &&
			      sippymodem_v34_descramble_group(
				      &descrambler, encoded, bit_count, &output) ==
				      SIPPYMODEM_V34_FRAMING_OK &&
			      output == input,
			      "V.34 group round trip changed at %u/%zu",
			      polynomial, index);
		}
		CHECK(scrambler.history == descrambler.history,
		      "V.34 framing histories diverged at %u", polynomial);
	}
	return true;
}

static bool test_superframe_inversion_patterns(void)
{
	static const uint8_t expected_j7[] = {
		0, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0,
	};
	static const uint8_t expected_j8[] = {
		0, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 0,
	};
	static const struct {
		enum sippymodem_v34_inversion_pattern pattern;
		const uint8_t *bits;
		size_t bit_count;
	} fixtures[] = {
		{ SIPPYMODEM_V34_INVERSION_J7, expected_j7,
		  ARRAY_LENGTH(expected_j7) },
		{ SIPPYMODEM_V34_INVERSION_J8, expected_j8,
		  ARRAY_LENGTH(expected_j8) },
	};
	uint8_t bit = UINT8_MAX;

	CHECK(ARRAY_LENGTH(expected_j7) ==
		      SIPPYMODEM_V34_J7_INVERSION_BITS &&
	      ARRAY_LENGTH(expected_j8) ==
		      SIPPYMODEM_V34_J8_INVERSION_BITS,
	      "V.34 inversion pattern lengths changed");
	for (size_t fixture = 0; fixture < ARRAY_LENGTH(fixtures); ++fixture) {
		for (size_t index = 0; index < fixtures[fixture].bit_count;
		     ++index) {
			CHECK(sippymodem_v34_superframe_inversion_bit(
				      fixtures[fixture].pattern,
				      (unsigned int)index, &bit) ==
				      SIPPYMODEM_V34_FRAMING_OK &&
			      bit == fixtures[fixture].bits[index],
			      "V.34 inversion bit changed at %zu/%zu", fixture,
			      index);
		}
	}
	bit = UINT8_MAX;
	CHECK(sippymodem_v34_superframe_inversion_bit(
		      SIPPYMODEM_V34_INVERSION_J7,
		      SIPPYMODEM_V34_J7_INVERSION_BITS, &bit) ==
		      SIPPYMODEM_V34_FRAMING_INVALID_ARGUMENT &&
	      sippymodem_v34_superframe_inversion_bit(
		      SIPPYMODEM_V34_INVERSION_J8,
		      SIPPYMODEM_V34_J8_INVERSION_BITS, &bit) ==
		      SIPPYMODEM_V34_FRAMING_INVALID_ARGUMENT &&
	      sippymodem_v34_superframe_inversion_bit(
		      (enum sippymodem_v34_inversion_pattern)2, 0, &bit) ==
		      SIPPYMODEM_V34_FRAMING_INVALID_ARGUMENT &&
	      sippymodem_v34_superframe_inversion_bit(
		      SIPPYMODEM_V34_INVERSION_J7, 0, NULL) ==
		      SIPPYMODEM_V34_FRAMING_INVALID_ARGUMENT &&
	      bit == UINT8_MAX,
	      "V.34 inversion lookup accepted an invalid position");
	return true;
}

static bool test_validation(void)
{
	struct sippymodem_v34_scrambler scrambler;
	struct sippymodem_v34_scrambler saved;
	uint8_t output = UINT8_MAX;

	CHECK(sippymodem_v34_scrambler_init(
		      &scrambler, SIPPYMODEM_V34_POLYNOMIAL_GPC) ==
		      SIPPYMODEM_V34_FRAMING_OK &&
	      sippymodem_v34_scrambler_is_valid(&scrambler) &&
	      !sippymodem_v34_scrambler_is_valid(NULL),
	      "V.34 validation fixture did not initialize");
	saved = scrambler;
	CHECK(sippymodem_v34_scrambler_init(
		      NULL, SIPPYMODEM_V34_POLYNOMIAL_GPC) ==
		      SIPPYMODEM_V34_FRAMING_INVALID_ARGUMENT &&
	      sippymodem_v34_scrambler_init(
		      &scrambler,
		      (enum sippymodem_v34_scrambler_polynomial)2) ==
		      SIPPYMODEM_V34_FRAMING_INVALID_ARGUMENT &&
	      memcmp(&scrambler, &saved, sizeof(scrambler)) == 0 &&
	      sippymodem_v34_scramble_group(
		      &scrambler, 0, 0, &output) ==
		      SIPPYMODEM_V34_FRAMING_INVALID_ARGUMENT &&
	      sippymodem_v34_scramble_group(
		      &scrambler, 0, 9, &output) ==
		      SIPPYMODEM_V34_FRAMING_INVALID_ARGUMENT &&
	      sippymodem_v34_scramble_group(
		      &scrambler, 8, 3, &output) ==
		      SIPPYMODEM_V34_FRAMING_INVALID_ARGUMENT &&
	      sippymodem_v34_descramble_group(
		      &scrambler, 0, 1, NULL) ==
		      SIPPYMODEM_V34_FRAMING_INVALID_ARGUMENT &&
	      memcmp(&scrambler, &saved, sizeof(scrambler)) == 0 &&
	      output == UINT8_MAX,
	      "V.34 framing accepted invalid input or mutated state");
	scrambler.history = UINT32_C(1) << SIPPYMODEM_V34_SCRAMBLER_BITS;
	CHECK(!sippymodem_v34_scrambler_is_valid(&scrambler),
	      "V.34 framing accepted corrupt history");
	scrambler.history = 0;
	scrambler.polynomial =
		(enum sippymodem_v34_scrambler_polynomial)2;
	CHECK(!sippymodem_v34_scrambler_is_valid(&scrambler) &&
		      strcmp(sippymodem_v34_framing_result_name(
			     SIPPYMODEM_V34_FRAMING_INVALID_ARGUMENT),
			     "invalid argument") == 0 &&
		      strcmp(sippymodem_v34_framing_result_name(
			     (enum sippymodem_v34_framing_result)2),
			     "unknown") == 0,
	      "V.34 framing accepted corrupt polynomial or result");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "polynomial starts", test_polynomial_starts },
		{ "station role polynomials", test_station_role_polynomials },
		{ "group round trip", test_group_round_trip },
		{ "superframe inversion patterns",
		  test_superframe_inversion_patterns },
		{ "validation", test_validation },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		bool passed;

		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		passed = tests[index].run();
		if (!passed) {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failures++;
		} else {
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		}
	}
	return failures == 0 ? 0 : 1;
}
