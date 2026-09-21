// SPDX-License-Identifier: BSD-3-Clause

#include <v34_framing.h>

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
		[V34_POLYNOMIAL_GPC] = {
			3, 3, 3, 3, 3, 3, 3, 3, 3, 0, 0, 1, 3, 3, 3,
		},
		[V34_POLYNOMIAL_GPA] = {
			3, 3, 2, 0, 0, 3, 3, 2, 0, 0, 3, 2, 1, 3, 3,
		},
	};

	for (size_t polynomial = 0; polynomial < ARRAY_LENGTH(expected);
	     ++polynomial) {
		struct v34_scrambler scrambler;

		CHECK(v34_scrambler_init(
			      &scrambler,
			      (enum v34_scrambler_polynomial)
				      polynomial) == V34_FRAMING_OK &&
		      scrambler.history == 0 &&
		      scrambler.polynomial ==
			      (enum v34_scrambler_polynomial)
				      polynomial,
		      "V.34 polynomial fixture did not initialize");
		for (size_t index = 0; index < ARRAY_LENGTH(expected[polynomial]);
		     ++index) {
			uint8_t output;

			CHECK(v34_scramble_group(
				      &scrambler, 3, 2, &output) ==
				      V34_FRAMING_OK &&
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
		enum v34_station_role role;
		enum v34_scrambler_polynomial polynomial;
	} cases[] = {
		{ V34_CALLING_STATION,
		  V34_POLYNOMIAL_GPC },
		{ V34_ANSWERING_STATION,
		  V34_POLYNOMIAL_GPA },
	};
	struct v34_scrambler scrambler;
	struct v34_scrambler before;

	for (size_t index = 0; index < ARRAY_LENGTH(cases); ++index) {
		enum v34_station_role other =
			v34_station_peer_role(cases[index].role);

		CHECK(v34_transmit_scrambler_init(
			      &scrambler, cases[index].role) ==
			      V34_FRAMING_OK &&
		      scrambler.history == 0 &&
		      scrambler.polynomial == cases[index].polynomial &&
		      v34_transmit_scrambler_is_valid(
			      &scrambler, cases[index].role) &&
		      !v34_transmit_scrambler_is_valid(
			      &scrambler, other),
		      "V.34 station role %zu selected the wrong polynomial",
		      index);
	}
	before = scrambler;
	CHECK(v34_transmit_scrambler_init(
		      NULL, V34_CALLING_STATION) ==
		      V34_FRAMING_INVALID_ARGUMENT &&
	      v34_station_peer_role(V34_CALLING_STATION) ==
		      V34_ANSWERING_STATION &&
	      v34_station_peer_role(V34_ANSWERING_STATION) ==
		      V34_CALLING_STATION &&
	      !v34_station_role_is_valid(v34_station_peer_role(
		      (enum v34_station_role)99)) &&
	      v34_transmit_scrambler_init(
		      &scrambler, (enum v34_station_role)99) ==
		      V34_FRAMING_INVALID_ARGUMENT &&
	      memcmp(&scrambler, &before, sizeof(scrambler)) == 0 &&
	      !v34_transmit_scrambler_is_valid(
		      NULL, V34_CALLING_STATION) &&
	      !v34_transmit_scrambler_is_valid(
		      &scrambler, (enum v34_station_role)99),
	      "V.34 station scrambler accepted invalid input");
	return true;
}

static bool test_group_round_trip(void)
{
	uint32_t random = UINT32_C(0x34f2b917);

	for (unsigned int polynomial = 0; polynomial < 2; ++polynomial) {
		struct v34_scrambler scrambler;
		struct v34_scrambler descrambler;

		CHECK(v34_scrambler_init(
			      &scrambler,
			      (enum v34_scrambler_polynomial)
				      polynomial) == V34_FRAMING_OK &&
		      v34_scrambler_init(
			      &descrambler,
			      (enum v34_scrambler_polynomial)
				      polynomial) == V34_FRAMING_OK,
		      "V.34 round-trip fixtures did not initialize");
		for (size_t index = 0; index < 2048; ++index) {
			unsigned int bit_count = (unsigned int)(index % 8u) + 1u;
			uint8_t mask = (uint8_t)((1u << bit_count) - 1u);
			uint8_t input = (uint8_t)fixture_random(&random) & mask;
			uint8_t encoded;
			uint8_t output;

			CHECK(v34_scramble_group(
				      &scrambler, input, bit_count, &encoded) ==
				      V34_FRAMING_OK &&
			      v34_descramble_group(
				      &descrambler, encoded, bit_count, &output) ==
				      V34_FRAMING_OK &&
			      output == input,
			      "V.34 group round trip changed at %u/%zu",
			      polynomial, index);
		}
		CHECK(scrambler.history == descrambler.history,
		      "V.34 framing histories diverged at %u", polynomial);
	}
	return true;
}

static bool test_differential_dibits(void)
{
	static const enum v34_station_role roles[] = {
		V34_CALLING_STATION,
		V34_ANSWERING_STATION,
	};
	static const uint8_t increments[] = { 0, 2, 1, 3 };

	for (size_t role = 0; role < ARRAY_LENGTH(roles); ++role) {
		for (uint8_t input = 0; input < ARRAY_LENGTH(increments); ++input) {
			for (uint8_t previous = 0; previous < 4u; ++previous) {
				struct v34_scrambler scrambler;
				uint8_t rotation = UINT8_MAX;

				CHECK(v34_transmit_scrambler_init(
					      &scrambler, roles[role]) ==
					      V34_FRAMING_OK &&
				      v34_scramble_differential_dibit(
					      &scrambler, input, previous,
					      &rotation) ==
					      V34_FRAMING_OK &&
				      rotation ==
					      (uint8_t)((previous +
							 increments[input]) &
							3u) &&
				      scrambler.history == input,
				      "V.34 differential dibit changed at %zu/%u/%u",
				      role, input, previous);
			}
		}
	}
	return true;
}

static bool test_differential_dibit_round_trip(void)
{
	static const enum v34_station_role roles[] = {
		V34_CALLING_STATION,
		V34_ANSWERING_STATION,
	};
	uint32_t random = UINT32_C(0x42b9e1d7);

	for (size_t role = 0; role < ARRAY_LENGTH(roles); ++role) {
		for (uint8_t start = 0; start < 4u; ++start) {
			struct v34_scrambler scrambler;
			struct v34_scrambler descrambler;
			uint8_t previous = start;

			CHECK(v34_transmit_scrambler_init(
				      &scrambler, roles[role]) ==
				      V34_FRAMING_OK &&
			      v34_transmit_scrambler_init(
				      &descrambler, roles[role]) ==
				      V34_FRAMING_OK,
			      "V.34 differential round trip did not initialize");
			for (size_t index = 0; index < 1024; ++index) {
				uint8_t input =
					(uint8_t)(fixture_random(&random) & 3u);
				uint8_t current;
				uint8_t output;

				CHECK(v34_scramble_differential_dibit(
					      &scrambler, input, previous,
					      &current) ==
						      V34_FRAMING_OK &&
				      v34_descramble_differential_dibit(
					      &descrambler, previous, current,
					      &output) ==
						      V34_FRAMING_OK &&
				      output == input &&
				      descrambler.history == scrambler.history,
				      "V.34 differential round trip changed at %zu/%u/%zu",
				      role, start, index);
				previous = current;
			}
		}
	}
	return true;
}

static bool test_mapped_dibits(void)
{
	static const enum v34_station_role roles[] = {
		V34_CALLING_STATION,
		V34_ANSWERING_STATION,
	};
	static const uint8_t expected[] = { 0, 2, 1, 3 };

	for (size_t role = 0; role < ARRAY_LENGTH(roles); ++role) {
		for (uint8_t input = 0; input < ARRAY_LENGTH(expected); ++input) {
			struct v34_scrambler scrambler;
			uint8_t mapped = UINT8_MAX;

			CHECK(v34_transmit_scrambler_init(
				      &scrambler, roles[role]) ==
				      V34_FRAMING_OK &&
			      v34_scramble_mapped_dibit(
				      &scrambler, input, &mapped) ==
				      V34_FRAMING_OK &&
			      mapped == expected[input] &&
			      scrambler.history == input,
			      "V.34 mapped dibit changed at %zu/%u", role,
			      input);
		}
	}
	return true;
}

static bool test_mapped_dibit_round_trip(void)
{
	static const enum v34_station_role roles[] = {
		V34_CALLING_STATION,
		V34_ANSWERING_STATION,
	};
	uint32_t random = UINT32_C(0x9127c43b);

	for (size_t role = 0; role < ARRAY_LENGTH(roles); ++role) {
		struct v34_scrambler scrambler;
		struct v34_scrambler descrambler;

		CHECK(v34_transmit_scrambler_init(
			      &scrambler, roles[role]) ==
			      V34_FRAMING_OK &&
		      v34_transmit_scrambler_init(
			      &descrambler, roles[role]) ==
			      V34_FRAMING_OK,
		      "V.34 mapped round trip did not initialize");
		for (size_t index = 0; index < 4096; ++index) {
			uint8_t input = (uint8_t)(fixture_random(&random) & 3u);
			uint8_t mapped;
			uint8_t output;

			CHECK(v34_scramble_mapped_dibit(
				      &scrambler, input, &mapped) ==
					      V34_FRAMING_OK &&
			      v34_descramble_mapped_dibit(
				      &descrambler, mapped, &output) ==
					      V34_FRAMING_OK &&
			      output == input &&
			      descrambler.history == scrambler.history,
			      "V.34 mapped round trip changed at %zu/%zu", role,
			      index);
		}
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
		enum v34_inversion_pattern pattern;
		const uint8_t *bits;
		size_t bit_count;
	} fixtures[] = {
		{ V34_INVERSION_J7, expected_j7,
		  ARRAY_LENGTH(expected_j7) },
		{ V34_INVERSION_J8, expected_j8,
		  ARRAY_LENGTH(expected_j8) },
	};
	uint8_t bit = UINT8_MAX;

	CHECK(ARRAY_LENGTH(expected_j7) ==
		      V34_J7_INVERSION_BITS &&
	      ARRAY_LENGTH(expected_j8) ==
		      V34_J8_INVERSION_BITS,
	      "V.34 inversion pattern lengths changed");
	for (size_t fixture = 0; fixture < ARRAY_LENGTH(fixtures); ++fixture) {
		for (size_t index = 0; index < fixtures[fixture].bit_count;
		     ++index) {
			CHECK(v34_superframe_inversion_bit(
				      fixtures[fixture].pattern,
				      (unsigned int)index, &bit) ==
				      V34_FRAMING_OK &&
			      bit == fixtures[fixture].bits[index],
			      "V.34 inversion bit changed at %zu/%zu", fixture,
			      index);
		}
	}
	bit = UINT8_MAX;
	CHECK(v34_superframe_inversion_bit(
		      V34_INVERSION_J7,
		      V34_J7_INVERSION_BITS, &bit) ==
		      V34_FRAMING_INVALID_ARGUMENT &&
	      v34_superframe_inversion_bit(
		      V34_INVERSION_J8,
		      V34_J8_INVERSION_BITS, &bit) ==
		      V34_FRAMING_INVALID_ARGUMENT &&
	      v34_superframe_inversion_bit(
		      (enum v34_inversion_pattern)2, 0, &bit) ==
		      V34_FRAMING_INVALID_ARGUMENT &&
	      v34_superframe_inversion_bit(
		      V34_INVERSION_J7, 0, NULL) ==
		      V34_FRAMING_INVALID_ARGUMENT &&
	      bit == UINT8_MAX,
	      "V.34 inversion lookup accepted an invalid position");
	return true;
}

static bool test_group_state_rejection(void)
{
	for (unsigned int direction = 0; direction < 2; ++direction) {
		for (unsigned int bad = 0; bad < 3; ++bad) {
			struct v34_scrambler state = {
				.history = bad == 0 ? UINT32_C(1) << V34_SCRAMBLER_BITS : 0,
				.polynomial = bad == 1 ? (enum v34_scrambler_polynomial)2 :
					V34_POLYNOMIAL_GPC,
			};
			struct v34_scrambler saved = state;
			struct v34_scrambler *owner = bad == 2 ? NULL : &state;
			uint8_t output = UINT8_MAX;
			enum v34_framing_result result = direction == 0 ?
				v34_scramble_group(owner, 1, 1, &output) :
				v34_descramble_group(owner, 1, 1, &output);

			CHECK(result == V34_FRAMING_INVALID_ARGUMENT &&
			      output == UINT8_MAX &&
			      memcmp(&state, &saved, sizeof(state)) == 0,
			      "V.34 group accepted invalid owner %u/%u", direction, bad);
		}
	}
	return true;
}

static bool test_validation(void)
{
	struct v34_scrambler scrambler;
	struct v34_scrambler saved;
	uint8_t output = UINT8_MAX;
	uint8_t rotation = UINT8_MAX;

	CHECK(v34_scrambler_init(
		      &scrambler, V34_POLYNOMIAL_GPC) ==
		      V34_FRAMING_OK &&
	      v34_scrambler_is_valid(&scrambler) &&
	      !v34_scrambler_is_valid(NULL),
	      "V.34 validation fixture did not initialize");
	saved = scrambler;
	CHECK(v34_scrambler_init(
		      NULL, V34_POLYNOMIAL_GPC) ==
		      V34_FRAMING_INVALID_ARGUMENT &&
	      v34_scrambler_init(
		      &scrambler,
		      (enum v34_scrambler_polynomial)2) ==
		      V34_FRAMING_INVALID_ARGUMENT &&
	      memcmp(&scrambler, &saved, sizeof(scrambler)) == 0 &&
	      v34_scramble_group(
		      &scrambler, 0, 0, &output) ==
		      V34_FRAMING_INVALID_ARGUMENT &&
	      v34_scramble_group(
		      &scrambler, 0, 9, &output) ==
		      V34_FRAMING_INVALID_ARGUMENT &&
	      v34_scramble_group(
		      &scrambler, 8, 3, &output) ==
		      V34_FRAMING_INVALID_ARGUMENT &&
	      v34_descramble_group(
		      &scrambler, 0, 1, NULL) ==
		      V34_FRAMING_INVALID_ARGUMENT &&
	      v34_scramble_mapped_dibit(
		      NULL, 0, &output) ==
		      V34_FRAMING_INVALID_ARGUMENT &&
	      v34_scramble_mapped_dibit(
		      &scrambler, 4, &output) ==
		      V34_FRAMING_INVALID_ARGUMENT &&
	      v34_scramble_mapped_dibit(
		      &scrambler, 0, NULL) ==
		      V34_FRAMING_INVALID_ARGUMENT &&
	      v34_descramble_mapped_dibit(
		      NULL, 0, &output) ==
		      V34_FRAMING_INVALID_ARGUMENT &&
	      v34_descramble_mapped_dibit(
		      &scrambler, 4, &output) ==
		      V34_FRAMING_INVALID_ARGUMENT &&
	      v34_descramble_mapped_dibit(
		      &scrambler, 0, NULL) ==
		      V34_FRAMING_INVALID_ARGUMENT &&
	      v34_scramble_differential_dibit(
		      NULL, 0, 0, &rotation) ==
		      V34_FRAMING_INVALID_ARGUMENT &&
	      v34_scramble_differential_dibit(
		      &scrambler, 4, 0, &rotation) ==
		      V34_FRAMING_INVALID_ARGUMENT &&
	      v34_scramble_differential_dibit(
		      &scrambler, 0, 4, &rotation) ==
		      V34_FRAMING_INVALID_ARGUMENT &&
	      v34_scramble_differential_dibit(
		      &scrambler, 0, 0, NULL) ==
		      V34_FRAMING_INVALID_ARGUMENT &&
	      v34_descramble_differential_dibit(
		      NULL, 0, 0, &output) ==
		      V34_FRAMING_INVALID_ARGUMENT &&
	      v34_descramble_differential_dibit(
		      &scrambler, 4, 0, &output) ==
		      V34_FRAMING_INVALID_ARGUMENT &&
	      v34_descramble_differential_dibit(
		      &scrambler, 0, 4, &output) ==
		      V34_FRAMING_INVALID_ARGUMENT &&
	      v34_descramble_differential_dibit(
		      &scrambler, 0, 0, NULL) ==
		      V34_FRAMING_INVALID_ARGUMENT &&
	      memcmp(&scrambler, &saved, sizeof(scrambler)) == 0 &&
	      output == UINT8_MAX && rotation == UINT8_MAX,
	      "V.34 framing accepted invalid input or mutated state");
	scrambler.history = UINT32_C(1) << V34_SCRAMBLER_BITS;
	CHECK(!v34_scrambler_is_valid(&scrambler),
	      "V.34 framing accepted corrupt history");
	scrambler.history = 0;
	scrambler.polynomial =
		(enum v34_scrambler_polynomial)2;
	CHECK(!v34_scrambler_is_valid(&scrambler),
	      "V.34 framing accepted corrupt polynomial");
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
		{ "mapped dibits", test_mapped_dibits },
		{ "mapped dibit round trip", test_mapped_dibit_round_trip },
		{ "differential dibits", test_differential_dibits },
		{ "differential dibit round trip",
		  test_differential_dibit_round_trip },
		{ "superframe inversion patterns",
		  test_superframe_inversion_patterns },
		{ "group state rejection", test_group_state_rejection },
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
