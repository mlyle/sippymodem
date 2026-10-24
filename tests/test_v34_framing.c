// SPDX-License-Identifier: BSD-3-Clause

#include <v34_framing.h>

#include "test.h"

#include <string.h>

static uint32_t fixture_random(uint32_t *state)
{
	*state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
	return *state;
}

static const uint8_t polynomial_start_dibits[][15] = {
	[V34_POLYNOMIAL_GPC] = {
		3, 3, 3, 3, 3, 3, 3, 3, 3, 0, 0, 1, 3, 3, 3,
	},
	[V34_POLYNOMIAL_GPA] = {
		3, 3, 2, 0, 0, 3, 3, 2, 0, 0, 3, 2, 1, 3, 3,
	},
};

static bool test_polynomial_starts(void)
{
	for (size_t polynomial = 0; polynomial < ARRAY_LENGTH(polynomial_start_dibits);
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
		for (size_t index = 0; index < ARRAY_LENGTH(polynomial_start_dibits[polynomial]);
		     ++index) {
			uint8_t output;

			CHECK(v34_scramble_group(
				      &scrambler, 3, 2, &output) ==
				      V34_FRAMING_OK &&
			      output == polynomial_start_dibits[polynomial][index],
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

static bool test_unpacked_bit_batches(void)
{
	uint32_t random = UINT32_C(0x9072a4b1);

	for (unsigned int polynomial = 0; polynomial < 2u; ++polynomial) {
		for (size_t count = 1u; count <= 80u; ++count) {
			struct v34_scrambler grouped, batched, descrambler;
			uint8_t input[80], expected[80], encoded[80];

			CHECK(v34_scrambler_init(&grouped, polynomial) == V34_FRAMING_OK &&
			      v34_scrambler_init(&batched, polynomial) == V34_FRAMING_OK &&
			      v34_scrambler_init(&descrambler, polynomial) == V34_FRAMING_OK,
			      "V.34 batch fixture failed at %u/%zu", polynomial, count);
			grouped.history = fixture_random(&random) &
				((UINT32_C(1) << V34_SCRAMBLER_BITS) - 1u);
			batched = descrambler = grouped;
			for (size_t bit = 0; bit < count; ++bit) {
				input[bit] = (uint8_t)(fixture_random(&random) >> 31);
				CHECK(v34_scramble_group(
					      &grouped, input[bit], 1u, &expected[bit]) ==
					      V34_FRAMING_OK,
				      "V.34 group fixture failed at %u/%zu", polynomial, count);
			}
			CHECK(v34_scramble_unpacked_bits(
				      &batched, input, count, encoded) == V34_FRAMING_OK &&
			      memcmp(encoded, expected, count) == 0 &&
			      batched.history == grouped.history,
			      "V.34 batch scramble changed at %u/%zu", polynomial, count);
			CHECK(v34_descramble_unpacked_bits(
				      &descrambler, encoded, count, encoded) == V34_FRAMING_OK &&
			      memcmp(encoded, input, count) == 0 &&
			      descrambler.history == grouped.history,
			      "V.34 in-place batch descramble changed at %u/%zu",
			      polynomial, count);
		}
		struct v34_scrambler scrambler;
		uint8_t input[9] = { 0 }, output[9];
		uint32_t before;

		CHECK(v34_scrambler_init(&scrambler, polynomial) == V34_FRAMING_OK,
		      "V.34 rejected batch fixture failed");
		before = scrambler.history;
		memset(output, 0xa5, sizeof(output));
		input[8] = 2u;
		CHECK(v34_scramble_unpacked_bits(
			      &scrambler, input, sizeof(input), output) ==
			      V34_FRAMING_INVALID_ARGUMENT &&
		      scrambler.history == before && output[0] == 0xa5 &&
		      output[8] == 0xa5,
		      "V.34 batch accepted a late invalid bit");
		input[8] = 0u;
		CHECK(v34_scramble_unpacked_bits(
			      &scrambler, input, 0u, output) ==
			      V34_FRAMING_INVALID_ARGUMENT &&
		      v34_descramble_unpacked_bits(
			      &scrambler, input, 8u, input + 1u) ==
			      V34_FRAMING_INVALID_ARGUMENT &&
		      scrambler.history == before && input[1] == 0u,
		      "V.34 batch accepted empty or overlapping input");
		struct v34_scrambler saved = scrambler;
		for (size_t byte = 0; byte < sizeof(scrambler); ++byte) {
			CHECK(v34_scramble_unpacked_bits(&scrambler, input, 1,
				      (uint8_t *)&scrambler + byte) ==
			      V34_FRAMING_INVALID_ARGUMENT &&
			      memcmp(&scrambler, &saved, sizeof(saved)) == 0,
			      "V.34 batch overwrote owner byte %zu", byte);
		}
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

static bool test_control_symbols(void)
{
	/* Clause 7's zero-seeded scramblers driven by ones, before the
	 * 10.1.3.9 differential and quarter-point bit mapping. */
	static const uint8_t mapped_dibits[] = { 0, 2, 1, 3 };

	for (unsigned int polynomial = 0; polynomial < ARRAY_LENGTH(polynomial_start_dibits);
	     ++polynomial) {
		for (unsigned int count = 2; count <= 4; count += 2) {
			for (uint8_t start = 0; start < 4; ++start) {
				struct v34_scrambler receiver;
				uint8_t previous = start;
				unsigned int stride = count / 2;

				CHECK(v34_scrambler_init(&receiver,
					      (enum v34_scrambler_polynomial)polynomial) ==
					      V34_FRAMING_OK,
				      "control symbol fixture did not initialize");
				for (size_t i = 0; i + stride <= ARRAY_LENGTH(polynomial_start_dibits[0]);
				     i += stride) {
					uint8_t current = (uint8_t)((previous +
						mapped_dibits[polynomial_start_dibits[polynomial][i]]) & 3u);
					uint8_t quarter = count == 4 ?
						mapped_dibits[polynomial_start_dibits[polynomial][i + 1]] : 0;
					uint8_t output = 0;

					CHECK(v34_descramble_control_symbol(&receiver,
						      previous, current, quarter, count, &output) ==
						      V34_FRAMING_OK &&
					      output == (uint8_t)((1u << count) - 1u),
					      "control symbol bit order changed at %u/%u/%u/%zu",
					      polynomial, count, start, i);
					previous = current;
				}
			}
		}
	}
	return true;
}

static bool test_control_symbol_rejection(void)
{
	static const struct {
		uint8_t previous, current, quarter;
		unsigned int bits;
	} cases[] = {
		{ 4, 0, 0, 2 }, { 0, 4, 0, 2 }, { 0, 0, 1, 2 },
		{ 0, 0, 4, 4 }, { 0, 0, 0, 0 }, { 0, 0, 0, 3 },
		{ 0, 0, 0, 8 },
	};
	struct v34_scrambler receiver, saved;
	uint8_t output = UINT8_MAX;

	CHECK(v34_scrambler_init(&receiver, V34_POLYNOMIAL_GPA) == V34_FRAMING_OK,
	      "control symbol rejection fixture did not initialize");
	receiver.history = UINT32_C(0x5a327);
	saved = receiver;
	for (size_t i = 0; i < ARRAY_LENGTH(cases); ++i)
		CHECK(v34_descramble_control_symbol(&receiver, cases[i].previous,
			      cases[i].current, cases[i].quarter, cases[i].bits,
			      &output) == V34_FRAMING_INVALID_ARGUMENT &&
		      memcmp(&receiver, &saved, sizeof(receiver)) == 0 &&
		      output == UINT8_MAX,
		      "invalid control symbol %zu changed state or output", i);
	CHECK(v34_descramble_control_symbol(NULL, 0, 0, 0, 2, &output) ==
		      V34_FRAMING_INVALID_ARGUMENT &&
	      v34_descramble_control_symbol(&receiver, 0, 0, 0, 4, NULL) ==
		      V34_FRAMING_INVALID_ARGUMENT &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0 &&
	      output == UINT8_MAX,
	      "null control symbol argument changed state or output");
	receiver.history = UINT32_C(1) << V34_SCRAMBLER_BITS;
	saved = receiver;
	CHECK(v34_descramble_control_symbol(&receiver, 0, 0, 0, 4, &output) ==
		      V34_FRAMING_INVALID_ARGUMENT &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0 &&
	      output == UINT8_MAX,
	      "corrupt control descrambler changed state or output");
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

static bool test_state_rejection(void)
{
	for (unsigned int operation = 0; operation < 6; ++operation) {
		for (unsigned int bad = 0; bad < 3; ++bad) {
			struct v34_scrambler state = {
				.history = bad == 0 ? UINT32_C(1) << V34_SCRAMBLER_BITS : 0,
				.polynomial = bad == 1 ? (enum v34_scrambler_polynomial)2 :
					V34_POLYNOMIAL_GPC,
			};
			struct v34_scrambler saved = state;
			struct v34_scrambler *owner = bad == 2 ? NULL : &state;
			uint8_t output = UINT8_MAX;
			enum v34_framing_result result;

			if (operation < 2) {
				result = operation == 0 ?
					v34_scramble_group(owner, 1, 1, &output) :
					v34_descramble_group(owner, 1, 1, &output);
			} else if (operation < 4) {
				result = operation == 2 ?
					v34_scramble_mapped_dibit(owner, 1, &output) :
					v34_descramble_mapped_dibit(owner, 1, &output);
			} else {
				result = operation == 4 ?
					v34_scramble_differential_dibit(owner, 1, 2, &output) :
					v34_descramble_differential_dibit(owner, 1, 2, &output);
			}

			CHECK(result == V34_FRAMING_INVALID_ARGUMENT &&
			      output == UINT8_MAX &&
			      memcmp(&state, &saved, sizeof(state)) == 0,
			      "V.34 framing changed invalid owner %u/%u", operation, bad);
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
		{ "unpacked bit batches", test_unpacked_bit_batches },
		{ "mapped dibits", test_mapped_dibits },
		{ "mapped dibit round trip", test_mapped_dibit_round_trip },
		{ "differential dibits", test_differential_dibits },
		{ "differential dibit round trip",
		  test_differential_dibit_round_trip },
		{ "control symbols", test_control_symbols },
		{ "control symbol rejection", test_control_symbol_rejection },
		{ "superframe inversion patterns",
		  test_superframe_inversion_patterns },
		{ "scrambler state rejection", test_state_rejection },
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
