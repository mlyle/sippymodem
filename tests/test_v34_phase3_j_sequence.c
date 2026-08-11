// SPDX-License-Identifier: BSD-3-Clause

#include <v34_phase3_j_sequence.h>
#include <v34_phase3_trn.h>

#include "test.h"

#include <math.h>
#include <string.h>

enum {
	REFERENCE_HISTORY_BITS = 23,
	J_PATTERN_REPETITIONS = 64,
	TRN_PREFIX_SYMBOLS = 613,
};

static const double TEST_PI =
	3.1415926535897932384626433832795;

struct reference_scrambler {
	uint8_t history[REFERENCE_HISTORY_BITS];
	unsigned int first_tap_delay;
};

static void reference_scrambler_init(
	struct reference_scrambler *scrambler, uint32_t history,
	enum v34_station_role role)
{
	memset(scrambler, 0, sizeof(*scrambler));
	scrambler->first_tap_delay =
		role == V34_CALLING_STATION ? 18u : 5u;
	for (size_t delay = 0; delay < REFERENCE_HISTORY_BITS; ++delay)
		scrambler->history[delay] =
			(uint8_t)((history >> delay) & 1u);
}

static uint8_t reference_scramble_bit(
	struct reference_scrambler *scrambler, uint8_t input)
{
	uint8_t output = input ^
		scrambler->history[scrambler->first_tap_delay - 1u] ^
		scrambler->history[REFERENCE_HISTORY_BITS - 1u];

	for (size_t delay = REFERENCE_HISTORY_BITS - 1u; delay > 0; --delay)
		scrambler->history[delay] = scrambler->history[delay - 1u];
	scrambler->history[0] = output;
	return output;
}

static uint32_t reference_history(
	const struct reference_scrambler *scrambler)
{
	uint32_t history = 0;

	for (size_t delay = 0; delay < REFERENCE_HISTORY_BITS; ++delay)
		history |= (uint32_t)scrambler->history[delay] << delay;
	return history;
}

static const char *reference_pattern(
	enum v34_constellation_size constellation)
{
	switch (constellation) {
	case V34_CONSTELLATION_4_POINT:
		return "0000100110010001";
	case V34_CONSTELLATION_16_POINT:
		return "0000110110010001";
	}
	return NULL;
}

static bool prepare_trn(
	struct v34_phase3_trn *trn,
	enum v34_station_role role, unsigned int symbols)
{
	struct sippymodem_qam_point ignored_point;
	uint8_t ignored_rotation;

	if (v34_phase3_trn_init(trn, role) !=
	    V34_PHASE3_TRN_OK)
		return false;
	for (unsigned int symbol = 0; symbol < symbols; ++symbol) {
		if (v34_phase3_trn_next(
			    trn, &ignored_point, &ignored_rotation) !=
		    V34_PHASE3_TRN_OK)
			return false;
	}
	return true;
}

static bool test_patterns_and_phase(void)
{
	static const enum v34_station_role roles[] = {
		V34_CALLING_STATION,
		V34_ANSWERING_STATION,
	};
	static const enum v34_constellation_size constellations[] = {
		V34_CONSTELLATION_4_POINT,
		V34_CONSTELLATION_16_POINT,
	};

	CHECK(V34_PHASE3_J_PATTERN_BITS == 16 &&
	      V34_PHASE3_J_PATTERN_SYMBOLS == 8,
	      "J pattern dimensions changed");
	for (size_t role_index = 0; role_index < ARRAY_LENGTH(roles);
	     ++role_index) {
		struct v34_phase3_trn trn;
		uint8_t first_pattern[ARRAY_LENGTH(constellations)]
				     [V34_PHASE3_J_PATTERN_SYMBOLS];

		CHECK(prepare_trn(
			      &trn, roles[role_index],
			      TRN_PREFIX_SYMBOLS + (unsigned int)role_index * 37u),
		      "J role %zu TRN prefix failed", role_index);
		for (size_t constellation_index = 0;
		     constellation_index < ARRAY_LENGTH(constellations);
		     ++constellation_index) {
			enum v34_constellation_size constellation =
				constellations[constellation_index];
			struct v34_phase3_j_sequence sequence;
			struct reference_scrambler reference;
			const char *pattern = reference_pattern(constellation);
			uint8_t expected_rotation =
				trn.last_clockwise_quarter_turns;
			unsigned int rotation_counts[4] = { 0 };

			CHECK(pattern != NULL && strlen(pattern) ==
				      V34_PHASE3_J_PATTERN_BITS &&
			      v34_phase3_j_sequence_init_from_trn(
				      &sequence, &trn, constellation) ==
				      V34_PHASE3_J_SEQUENCE_OK &&
			      v34_phase3_j_sequence_is_valid(
				      &sequence) &&
			      v34_phase3_j_sequence_at_pattern_boundary(
				      &sequence) &&
			      sequence.scrambler.history ==
				      trn.scrambler.history &&
			      sequence.scrambler.polynomial ==
				      trn.scrambler.polynomial &&
			      sequence.role == trn.role &&
			      sequence.remote_constellation == constellation &&
			      sequence.previous_clockwise_quarter_turns ==
				      trn.last_clockwise_quarter_turns &&
			      sequence.symbol_in_pattern == 0,
			      "J role %zu constellation %zu handoff failed",
			      role_index, constellation_index);
			reference_scrambler_init(
				&reference, trn.scrambler.history, trn.role);
			for (unsigned int symbol = 0;
			     symbol < J_PATTERN_REPETITIONS *
					      V34_PHASE3_J_PATTERN_SYMBOLS;
			     ++symbol) {
				unsigned int pattern_symbol =
					symbol %
					V34_PHASE3_J_PATTERN_SYMBOLS;
				unsigned int bit = pattern_symbol * 2u;
				uint8_t i1 = reference_scramble_bit(
					&reference, (uint8_t)(pattern[bit] - '0'));
				uint8_t i2 = reference_scramble_bit(
					&reference,
					(uint8_t)(pattern[bit + 1u] - '0'));
				uint8_t increment = (uint8_t)((i2 << 1) | i1);
				struct sippymodem_qam_point point = { 7.0, -9.0 };
				uint8_t rotation = UINT8_MAX;
				double angle;
				double power;

				expected_rotation = (uint8_t)(
					(expected_rotation + increment) & 3u);
				CHECK(v34_phase3_j_sequence_at_pattern_boundary(
					      &sequence) == (pattern_symbol == 0) &&
				      v34_phase3_j_sequence_next(
					      &sequence, &point, &rotation) ==
					      V34_PHASE3_J_SEQUENCE_OK,
				      "J role %zu constellation %zu stopped at %u",
				      role_index, constellation_index, symbol);
				angle = TEST_PI / 4.0 -
					expected_rotation * TEST_PI / 2.0;
				power = point.in_phase * point.in_phase +
					point.quadrature * point.quadrature;
				CHECK(rotation == expected_rotation &&
				      sequence.previous_clockwise_quarter_turns ==
					      expected_rotation &&
				      sequence.symbol_in_pattern ==
					      (pattern_symbol + 1u) %
						      V34_PHASE3_J_PATTERN_SYMBOLS &&
				      v34_phase3_j_sequence_at_pattern_boundary(
					      &sequence) ==
					      (pattern_symbol + 1u ==
					       V34_PHASE3_J_PATTERN_SYMBOLS) &&
				      fabs(point.in_phase - cos(angle)) < 1e-14 &&
				      fabs(point.quadrature - sin(angle)) < 1e-14 &&
				      fabs(power - 1.0) < 1e-14,
				      "J role %zu constellation %zu symbol %u changed",
				      role_index, constellation_index, symbol);
				rotation_counts[rotation]++;
				if (symbol <
				    V34_PHASE3_J_PATTERN_SYMBOLS)
					first_pattern[constellation_index][symbol] =
						rotation;
			}
			CHECK(sequence.scrambler.history ==
				      reference_history(&reference) &&
			      rotation_counts[0] > 0 && rotation_counts[1] > 0 &&
			      rotation_counts[2] > 0 && rotation_counts[3] > 0,
			      "J role %zu constellation %zu lost state coverage",
			      role_index, constellation_index);
		}
		CHECK(memcmp(first_pattern[0], first_pattern[1],
			     sizeof(first_pattern[0])) != 0,
		      "J role %zu constellation patterns did not diverge",
		      role_index);
	}
	return true;
}

static bool test_copy_and_restart(void)
{
	struct v34_phase3_trn trn;
	struct v34_phase3_j_sequence first;
	struct v34_phase3_j_sequence second;
	struct v34_phase3_j_sequence restarted;
	struct sippymodem_qam_point ignored;
	uint8_t ignored_rotation;

	CHECK(prepare_trn(
		      &trn, V34_ANSWERING_STATION,
		      TRN_PREFIX_SYMBOLS) &&
	      v34_phase3_j_sequence_init_from_trn(
		      &first, &trn, V34_CONSTELLATION_16_POINT) ==
		      V34_PHASE3_J_SEQUENCE_OK,
	      "J copy fixture did not initialize");
	for (unsigned int symbol = 0; symbol < 5; ++symbol)
		CHECK(v34_phase3_j_sequence_next(
			      &first, &ignored, &ignored_rotation) ==
			      V34_PHASE3_J_SEQUENCE_OK,
		      "J copy prefix stopped at %u", symbol);
	second = first;
	for (unsigned int symbol = 0; symbol < 257; ++symbol) {
		struct sippymodem_qam_point first_point;
		struct sippymodem_qam_point second_point;
		uint8_t first_rotation;
		uint8_t second_rotation;

		CHECK(v34_phase3_j_sequence_next(
			      &first, &first_point, &first_rotation) ==
			      V34_PHASE3_J_SEQUENCE_OK &&
		      v34_phase3_j_sequence_next(
			      &second, &second_point, &second_rotation) ==
			      V34_PHASE3_J_SEQUENCE_OK &&
		      first_rotation == second_rotation &&
		      memcmp(&first_point, &second_point,
			     sizeof(first_point)) == 0 &&
		      memcmp(&first, &second, sizeof(first)) == 0,
		      "copied J stream diverged at %u", symbol);
	}
	CHECK(v34_phase3_j_sequence_init_from_trn(
		      &restarted, &trn,
		      V34_CONSTELLATION_16_POINT) ==
		      V34_PHASE3_J_SEQUENCE_OK &&
	      v34_phase3_j_sequence_at_pattern_boundary(
		      &restarted) &&
	      restarted.scrambler.history == trn.scrambler.history &&
	      restarted.previous_clockwise_quarter_turns ==
		      trn.last_clockwise_quarter_turns,
	      "restarted J did not return to the TRN boundary");
	return true;
}

static bool test_validation(void)
{
	struct v34_phase3_trn trn;
	struct v34_phase3_j_sequence sequence;
	struct v34_phase3_j_sequence before;
	struct v34_phase3_j_sequence corrupt;
	struct sippymodem_qam_point point = { 7.0, -9.0 };
	struct sippymodem_qam_point before_point = point;
	uint8_t rotation = UINT8_MAX;

	memset(&sequence, 0xa5, sizeof(sequence));
	before = sequence;
	CHECK(v34_phase3_trn_init(
		      &trn, V34_CALLING_STATION) ==
		      V34_PHASE3_TRN_OK &&
	      v34_phase3_j_sequence_init_from_trn(
		      NULL, &trn, V34_CONSTELLATION_4_POINT) ==
		      V34_PHASE3_J_SEQUENCE_INVALID_ARGUMENT &&
	      v34_phase3_j_sequence_init_from_trn(
		      &sequence, NULL,
		      V34_CONSTELLATION_4_POINT) ==
		      V34_PHASE3_J_SEQUENCE_INVALID_ARGUMENT &&
	      v34_phase3_j_sequence_init_from_trn(
		      &sequence, &trn,
		      V34_CONSTELLATION_4_POINT) ==
		      V34_PHASE3_J_SEQUENCE_INVALID_ARGUMENT &&
	      v34_phase3_j_sequence_init_from_trn(
		      &sequence, &trn,
		      (enum v34_constellation_size)8) ==
		      V34_PHASE3_J_SEQUENCE_INVALID_ARGUMENT &&
	      memcmp(&sequence, &before, sizeof(sequence)) == 0,
	      "invalid J handoff mutated its destination");
	CHECK(prepare_trn(
		      &trn, V34_CALLING_STATION,
		      V34_PHASE3_TRN_MIN_SYMBOLS) &&
	      v34_phase3_j_sequence_init_from_trn(
		      &sequence, &trn,
		      V34_CONSTELLATION_4_POINT) ==
		      V34_PHASE3_J_SEQUENCE_OK,
	      "J validation fixture did not initialize");
	before = sequence;
	CHECK(v34_phase3_j_sequence_next(
		      NULL, &point, &rotation) ==
		      V34_PHASE3_J_SEQUENCE_INVALID_ARGUMENT &&
	      v34_phase3_j_sequence_next(
		      &sequence, NULL, &rotation) ==
		      V34_PHASE3_J_SEQUENCE_INVALID_ARGUMENT &&
	      v34_phase3_j_sequence_next(
		      &sequence, &point, NULL) ==
		      V34_PHASE3_J_SEQUENCE_INVALID_ARGUMENT &&
	      memcmp(&sequence, &before, sizeof(sequence)) == 0 &&
	      memcmp(&point, &before_point, sizeof(point)) == 0 &&
	      rotation == UINT8_MAX,
	      "invalid J output argument mutated state");
	sequence.scrambler.history = UINT32_C(1) << 23;
	corrupt = sequence;
	CHECK(!v34_phase3_j_sequence_is_valid(&sequence) &&
	      !v34_phase3_j_sequence_at_pattern_boundary(&sequence) &&
	      v34_phase3_j_sequence_next(
		      &sequence, &point, &rotation) ==
		      V34_PHASE3_J_SEQUENCE_INVALID_ARGUMENT &&
	      memcmp(&sequence, &corrupt, sizeof(sequence)) == 0 &&
	      memcmp(&point, &before_point, sizeof(point)) == 0 &&
	      rotation == UINT8_MAX,
	      "J accepted corrupt scrambler history");
	sequence = before;
	sequence.scrambler.polynomial = V34_POLYNOMIAL_GPA;
	CHECK(!v34_phase3_j_sequence_is_valid(&sequence),
	      "J accepted a polynomial from the other station role");
	sequence = before;
	sequence.role = (enum v34_station_role)99;
	CHECK(!v34_phase3_j_sequence_is_valid(&sequence),
	      "J accepted an invalid station role");
	sequence = before;
	sequence.remote_constellation =
		(enum v34_constellation_size)8;
	CHECK(!v34_phase3_j_sequence_is_valid(&sequence),
	      "J accepted an invalid constellation");
	sequence = before;
	sequence.previous_clockwise_quarter_turns = 4;
	CHECK(!v34_phase3_j_sequence_is_valid(&sequence),
	      "J accepted an invalid differential phase");
	sequence = before;
	sequence.symbol_in_pattern =
		V34_PHASE3_J_PATTERN_SYMBOLS;
	CHECK(!v34_phase3_j_sequence_is_valid(&sequence) &&
	      !v34_phase3_j_sequence_is_valid(NULL) &&
	      !v34_phase3_j_sequence_at_pattern_boundary(NULL),
	      "J accepted an invalid pattern position");
	CHECK(strcmp(v34_phase3_j_sequence_result_name(
			     V34_PHASE3_J_SEQUENCE_INVALID_ARGUMENT),
		     "invalid argument") == 0 &&
	      strcmp(v34_phase3_j_sequence_result_name(
			     (enum v34_phase3_j_sequence_result)99),
		     "unknown") == 0,
	      "J result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "patterns and differential phase", test_patterns_and_phase },
		{ "copy and restart", test_copy_and_restart },
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
