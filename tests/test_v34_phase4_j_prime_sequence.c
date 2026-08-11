// SPDX-License-Identifier: BSD-3-Clause

#include <v34_phase3_j_sequence.h>
#include <v34_phase3_trn.h>
#include <v34_phase4_j_prime_sequence.h>

#include "test.h"

#include <math.h>
#include <string.h>

enum {
	REFERENCE_HISTORY_BITS = 23,
	TRN_PREFIX_SYMBOLS = 613,
	J_PREFIX_PATTERNS = 5,
};

static const double TEST_PI =
	3.1415926535897932384626433832795;
static const char J_PRIME_PATTERN[] = "1111100110010001";

struct reference_scrambler {
	uint8_t history[REFERENCE_HISTORY_BITS];
	unsigned int first_tap_delay;
};

static void reference_scrambler_init(
	struct reference_scrambler *scrambler, uint32_t history,
	enum sippymodem_v34_station_role role)
{
	memset(scrambler, 0, sizeof(*scrambler));
	scrambler->first_tap_delay =
		role == SIPPYMODEM_V34_CALLING_STATION ? 18u : 5u;
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

static bool prepare_j(
	struct sippymodem_v34_phase3_j_sequence *j_sequence,
	enum sippymodem_v34_station_role role,
	enum sippymodem_v34_constellation_size constellation,
	unsigned int patterns)
{
	struct sippymodem_v34_phase3_trn trn;
	struct sippymodem_qam_point ignored_point;
	uint8_t ignored_rotation;

	if (sippymodem_v34_phase3_trn_init(&trn, role) !=
	    SIPPYMODEM_V34_PHASE3_TRN_OK)
		return false;
	for (unsigned int symbol = 0; symbol < TRN_PREFIX_SYMBOLS; ++symbol) {
		if (sippymodem_v34_phase3_trn_next(
			    &trn, &ignored_point, &ignored_rotation) !=
		    SIPPYMODEM_V34_PHASE3_TRN_OK)
			return false;
	}
	if (sippymodem_v34_phase3_j_sequence_init_from_trn(
		    j_sequence, &trn, constellation) !=
	    SIPPYMODEM_V34_PHASE3_J_SEQUENCE_OK)
		return false;
	for (unsigned int symbol = 0;
	     symbol < patterns * SIPPYMODEM_V34_PHASE3_J_PATTERN_SYMBOLS;
	     ++symbol) {
		if (sippymodem_v34_phase3_j_sequence_next(
			    j_sequence, &ignored_point, &ignored_rotation) !=
		    SIPPYMODEM_V34_PHASE3_J_SEQUENCE_OK)
			return false;
	}
	return sippymodem_v34_phase3_j_sequence_at_pattern_boundary(j_sequence);
}

static bool test_normative_sequence(void)
{
	static const enum sippymodem_v34_constellation_size constellations[] = {
		SIPPYMODEM_V34_CONSTELLATION_4_POINT,
		SIPPYMODEM_V34_CONSTELLATION_16_POINT,
	};

	CHECK(strlen(J_PRIME_PATTERN) == SIPPYMODEM_V34_PHASE4_J_PRIME_BITS &&
	      SIPPYMODEM_V34_PHASE4_J_PRIME_BITS == 16 &&
	      SIPPYMODEM_V34_PHASE4_J_PRIME_SYMBOLS == 8,
	      "J-prime pattern dimensions changed");
	for (size_t selection = 0; selection < ARRAY_LENGTH(constellations);
	     ++selection) {
		struct sippymodem_v34_phase3_j_sequence j_sequence;
		struct sippymodem_v34_phase3_j_sequence saved_j;
		struct sippymodem_v34_phase4_j_prime_sequence sequence;
		struct sippymodem_v34_phase4_j_prime_sequence completed;
		struct reference_scrambler reference;
		uint8_t expected_rotation;

		CHECK(prepare_j(
			      &j_sequence, SIPPYMODEM_V34_CALLING_STATION,
			      constellations[selection],
			      J_PREFIX_PATTERNS + (unsigned int)selection),
		      "J-prime selection %zu prefix failed", selection);
		saved_j = j_sequence;
		CHECK(sippymodem_v34_phase4_j_prime_sequence_init_from_j(
			      &sequence, &j_sequence) ==
			      SIPPYMODEM_V34_PHASE4_J_PRIME_SEQUENCE_OK &&
		      sippymodem_v34_phase4_j_prime_sequence_is_valid(
			      &sequence) &&
		      !sippymodem_v34_phase4_j_prime_sequence_is_complete(
			      &sequence) &&
		      sequence.scrambler.history ==
			      j_sequence.scrambler.history &&
		      sequence.scrambler.polynomial ==
			      j_sequence.scrambler.polynomial &&
		      sequence.role == SIPPYMODEM_V34_CALLING_STATION &&
		      sequence.previous_clockwise_quarter_turns ==
			      j_sequence.previous_clockwise_quarter_turns &&
		      sequence.emitted_symbols == 0 &&
		      memcmp(&j_sequence, &saved_j, sizeof(j_sequence)) == 0,
		      "J-prime selection %zu handoff failed", selection);
		expected_rotation = j_sequence.previous_clockwise_quarter_turns;
		reference_scrambler_init(
			&reference, j_sequence.scrambler.history, j_sequence.role);
		for (unsigned int symbol = 0;
		     symbol < SIPPYMODEM_V34_PHASE4_J_PRIME_SYMBOLS; ++symbol) {
			unsigned int bit = symbol * 2u;
			uint8_t i1 = reference_scramble_bit(
				&reference,
				(uint8_t)(J_PRIME_PATTERN[bit] - '0'));
			uint8_t i2 = reference_scramble_bit(
				&reference,
				(uint8_t)(J_PRIME_PATTERN[bit + 1u] - '0'));
			struct sippymodem_qam_point point = { 7.0, -9.0 };
			uint8_t rotation = UINT8_MAX;
			double angle;
			double power;

			expected_rotation = (uint8_t)(
				(expected_rotation + (i2 << 1) + i1) & 3u);
			CHECK(sippymodem_v34_phase4_j_prime_sequence_next(
				      &sequence, &point, &rotation) ==
				      SIPPYMODEM_V34_PHASE4_J_PRIME_SEQUENCE_OK,
			      "J-prime selection %zu stopped at %u", selection,
			      symbol);
			angle = TEST_PI / 4.0 -
				expected_rotation * TEST_PI / 2.0;
			power = point.in_phase * point.in_phase +
				point.quadrature * point.quadrature;
			CHECK(rotation == expected_rotation &&
			      sequence.previous_clockwise_quarter_turns ==
				      expected_rotation &&
			      sequence.emitted_symbols == symbol + 1u &&
			      sippymodem_v34_phase4_j_prime_sequence_is_complete(
				      &sequence) ==
				      (symbol + 1u ==
				       SIPPYMODEM_V34_PHASE4_J_PRIME_SYMBOLS) &&
			      fabs(point.in_phase - cos(angle)) < 1e-14 &&
			      fabs(point.quadrature - sin(angle)) < 1e-14 &&
			      fabs(power - 1.0) < 1e-14,
			      "J-prime selection %zu symbol %u changed", selection,
			      symbol);
		}
		CHECK(sequence.scrambler.history ==
			      reference_history(&reference) &&
		      memcmp(&j_sequence, &saved_j, sizeof(j_sequence)) == 0,
		      "J-prime selection %zu lost continuation state",
		      selection);
		completed = sequence;
		{
			struct sippymodem_qam_point point = { 7.0, -9.0 };
			struct sippymodem_qam_point saved_point = point;
			uint8_t rotation = UINT8_MAX;

			CHECK(sippymodem_v34_phase4_j_prime_sequence_next(
				      &sequence, &point, &rotation) ==
				      SIPPYMODEM_V34_PHASE4_J_PRIME_SEQUENCE_COMPLETE &&
			      memcmp(&sequence, &completed, sizeof(sequence)) == 0 &&
			      memcmp(&point, &saved_point, sizeof(point)) == 0 &&
			      rotation == UINT8_MAX,
			      "completed J-prime selection %zu emitted twice",
			      selection);
		}
	}
	return true;
}

static bool test_copy_and_restart(void)
{
	struct sippymodem_v34_phase3_j_sequence j_sequence;
	struct sippymodem_v34_phase4_j_prime_sequence first;
	struct sippymodem_v34_phase4_j_prime_sequence second;
	struct sippymodem_v34_phase4_j_prime_sequence restarted;
	struct sippymodem_qam_point ignored;
	uint8_t ignored_rotation;

	CHECK(prepare_j(
		      &j_sequence, SIPPYMODEM_V34_CALLING_STATION,
		      SIPPYMODEM_V34_CONSTELLATION_4_POINT,
		      J_PREFIX_PATTERNS) &&
	      sippymodem_v34_phase4_j_prime_sequence_init_from_j(
		      &first, &j_sequence) ==
		      SIPPYMODEM_V34_PHASE4_J_PRIME_SEQUENCE_OK,
	      "J-prime copy fixture did not initialize");
	for (unsigned int symbol = 0; symbol < 3; ++symbol)
		CHECK(sippymodem_v34_phase4_j_prime_sequence_next(
			      &first, &ignored, &ignored_rotation) ==
			      SIPPYMODEM_V34_PHASE4_J_PRIME_SEQUENCE_OK,
		      "J-prime copy prefix stopped at %u", symbol);
	second = first;
	while (!sippymodem_v34_phase4_j_prime_sequence_is_complete(&first)) {
		struct sippymodem_qam_point first_point;
		struct sippymodem_qam_point second_point;
		uint8_t first_rotation;
		uint8_t second_rotation;

		CHECK(sippymodem_v34_phase4_j_prime_sequence_next(
			      &first, &first_point, &first_rotation) ==
			      SIPPYMODEM_V34_PHASE4_J_PRIME_SEQUENCE_OK &&
		      sippymodem_v34_phase4_j_prime_sequence_next(
			      &second, &second_point, &second_rotation) ==
			      SIPPYMODEM_V34_PHASE4_J_PRIME_SEQUENCE_OK &&
		      first_rotation == second_rotation &&
		      memcmp(&first_point, &second_point,
			     sizeof(first_point)) == 0 &&
		      memcmp(&first, &second, sizeof(first)) == 0,
		      "copied J-prime streams diverged");
	}
	CHECK(sippymodem_v34_phase4_j_prime_sequence_init_from_j(
		      &restarted, &j_sequence) ==
		      SIPPYMODEM_V34_PHASE4_J_PRIME_SEQUENCE_OK &&
	      restarted.scrambler.history == j_sequence.scrambler.history &&
	      restarted.previous_clockwise_quarter_turns ==
		      j_sequence.previous_clockwise_quarter_turns &&
	      restarted.emitted_symbols == 0,
	      "restarted J-prime did not return to the J boundary");
	return true;
}

static bool test_validation(void)
{
	struct sippymodem_v34_phase3_j_sequence calling_j;
	struct sippymodem_v34_phase3_j_sequence answering_j;
	struct sippymodem_v34_phase4_j_prime_sequence sequence;
	struct sippymodem_v34_phase4_j_prime_sequence before;
	struct sippymodem_v34_phase4_j_prime_sequence corrupt;
	struct sippymodem_qam_point point = { 7.0, -9.0 };
	struct sippymodem_qam_point saved_point = point;
	uint8_t rotation = UINT8_MAX;

	CHECK(prepare_j(
		      &calling_j, SIPPYMODEM_V34_CALLING_STATION,
		      SIPPYMODEM_V34_CONSTELLATION_4_POINT, 1) &&
	      prepare_j(
		      &answering_j, SIPPYMODEM_V34_ANSWERING_STATION,
		      SIPPYMODEM_V34_CONSTELLATION_4_POINT, 1),
	      "J-prime validation prefixes failed");
	memset(&sequence, 0xa5, sizeof(sequence));
	before = sequence;
	CHECK(sippymodem_v34_phase4_j_prime_sequence_init_from_j(
		      NULL, &calling_j) ==
		      SIPPYMODEM_V34_PHASE4_J_PRIME_SEQUENCE_INVALID_ARGUMENT &&
	      sippymodem_v34_phase4_j_prime_sequence_init_from_j(
		      &sequence, NULL) ==
		      SIPPYMODEM_V34_PHASE4_J_PRIME_SEQUENCE_INVALID_ARGUMENT &&
	      sippymodem_v34_phase4_j_prime_sequence_init_from_j(
		      &sequence, &answering_j) ==
		      SIPPYMODEM_V34_PHASE4_J_PRIME_SEQUENCE_INVALID_ARGUMENT &&
	      memcmp(&sequence, &before, sizeof(sequence)) == 0,
	      "invalid J-prime handoff mutated its destination");
	CHECK(sippymodem_v34_phase3_j_sequence_next(
		      &calling_j, &point, &rotation) ==
		      SIPPYMODEM_V34_PHASE3_J_SEQUENCE_OK &&
	      sippymodem_v34_phase4_j_prime_sequence_init_from_j(
		      &sequence, &calling_j) ==
		      SIPPYMODEM_V34_PHASE4_J_PRIME_SEQUENCE_INVALID_ARGUMENT &&
	      memcmp(&sequence, &before, sizeof(sequence)) == 0,
	      "J-prime accepted a partial J pattern");
	CHECK(prepare_j(
		      &calling_j, SIPPYMODEM_V34_CALLING_STATION,
		      SIPPYMODEM_V34_CONSTELLATION_16_POINT, 2) &&
	      sippymodem_v34_phase4_j_prime_sequence_init_from_j(
		      &sequence, &calling_j) ==
		      SIPPYMODEM_V34_PHASE4_J_PRIME_SEQUENCE_OK,
	      "J-prime validation fixture did not initialize");
	point = saved_point;
	rotation = UINT8_MAX;
	before = sequence;
	CHECK(sippymodem_v34_phase4_j_prime_sequence_next(
		      NULL, &point, &rotation) ==
		      SIPPYMODEM_V34_PHASE4_J_PRIME_SEQUENCE_INVALID_ARGUMENT &&
	      sippymodem_v34_phase4_j_prime_sequence_next(
		      &sequence, NULL, &rotation) ==
		      SIPPYMODEM_V34_PHASE4_J_PRIME_SEQUENCE_INVALID_ARGUMENT &&
	      sippymodem_v34_phase4_j_prime_sequence_next(
		      &sequence, &point, NULL) ==
		      SIPPYMODEM_V34_PHASE4_J_PRIME_SEQUENCE_INVALID_ARGUMENT &&
	      memcmp(&sequence, &before, sizeof(sequence)) == 0 &&
	      memcmp(&point, &saved_point, sizeof(point)) == 0 &&
	      rotation == UINT8_MAX,
	      "invalid J-prime output argument mutated state");
	sequence.scrambler.history = UINT32_C(1) << 23;
	corrupt = sequence;
	CHECK(!sippymodem_v34_phase4_j_prime_sequence_is_valid(&sequence) &&
	      !sippymodem_v34_phase4_j_prime_sequence_is_complete(&sequence) &&
	      sippymodem_v34_phase4_j_prime_sequence_next(
		      &sequence, &point, &rotation) ==
		      SIPPYMODEM_V34_PHASE4_J_PRIME_SEQUENCE_INVALID_ARGUMENT &&
	      memcmp(&sequence, &corrupt, sizeof(sequence)) == 0 &&
	      memcmp(&point, &saved_point, sizeof(point)) == 0 &&
	      rotation == UINT8_MAX,
	      "J-prime accepted corrupt scrambler history");
	sequence = before;
	sequence.scrambler.polynomial = SIPPYMODEM_V34_POLYNOMIAL_GPA;
	CHECK(!sippymodem_v34_phase4_j_prime_sequence_is_valid(&sequence),
	      "J-prime accepted the answering-station polynomial");
	sequence = before;
	sequence.role = SIPPYMODEM_V34_ANSWERING_STATION;
	CHECK(!sippymodem_v34_phase4_j_prime_sequence_is_valid(&sequence),
	      "J-prime accepted the answering station");
	sequence = before;
	sequence.previous_clockwise_quarter_turns = 4;
	CHECK(!sippymodem_v34_phase4_j_prime_sequence_is_valid(&sequence),
	      "J-prime accepted an invalid differential phase");
	sequence = before;
	sequence.emitted_symbols = SIPPYMODEM_V34_PHASE4_J_PRIME_SYMBOLS + 1u;
	CHECK(!sippymodem_v34_phase4_j_prime_sequence_is_valid(&sequence) &&
	      !sippymodem_v34_phase4_j_prime_sequence_is_valid(NULL) &&
	      !sippymodem_v34_phase4_j_prime_sequence_is_complete(NULL),
	      "J-prime accepted an invalid symbol count");
	CHECK(strcmp(sippymodem_v34_phase4_j_prime_sequence_result_name(
			     SIPPYMODEM_V34_PHASE4_J_PRIME_SEQUENCE_COMPLETE),
		     "complete") == 0 &&
	      strcmp(sippymodem_v34_phase4_j_prime_sequence_result_name(
			     SIPPYMODEM_V34_PHASE4_J_PRIME_SEQUENCE_INVALID_ARGUMENT),
		     "invalid argument") == 0 &&
	      strcmp(sippymodem_v34_phase4_j_prime_sequence_result_name(
			     (enum sippymodem_v34_phase4_j_prime_sequence_result)99),
		     "unknown") == 0,
	      "J-prime result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "normative sequence", test_normative_sequence },
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
