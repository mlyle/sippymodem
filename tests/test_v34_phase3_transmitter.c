// SPDX-License-Identifier: BSD-3-Clause

#include <v34_phase3_transmitter.h>

#include "test.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static bool points_match(
	const struct qam_point *left, const struct qam_point *right)
{
	return left->in_phase == right->in_phase &&
	       left->quadrature == right->quadrature;
}

static bool scramblers_match(
	const struct v34_scrambler *left,
	const struct v34_scrambler *right)
{
	return left->history == right->history &&
	       left->polynomial == right->polynomial;
}

static bool trn_matches(
	const struct v34_training_sequence *left,
	const struct v34_training_sequence *right)
{
	return scramblers_match(&left->scrambler, &right->scrambler) &&
	       left->role == right->role &&
	       left->constellation == right->constellation &&
	       left->last_clockwise_quarter_turns ==
		       right->last_clockwise_quarter_turns &&
	       left->has_last_symbol == right->has_last_symbol;
}

static bool j_matches(
	const struct v34_phase3_j_sequence *left,
	const struct v34_phase3_j_sequence *right)
{
	return scramblers_match(&left->scrambler, &right->scrambler) &&
	       left->role == right->role &&
	       left->remote_constellation == right->remote_constellation &&
	       left->previous_clockwise_quarter_turns ==
		       right->previous_clockwise_quarter_turns &&
	       left->symbol_in_pattern == right->symbol_in_pattern;
}

static bool run_sequence(
	enum v34_station_role role,
	enum v34_constellation_size constellation)
{
	struct v34_phase3_transmitter transmitter;
	struct v34_phase3_s_sequence expected_s;
	struct v34_phase3_pp_sequence expected_pp;
	struct v34_training_sequence expected_trn;
	struct v34_phase3_j_sequence expected_j;
	struct v34_phase4_j_prime_sequence j_prime;
	struct v34_phase4_j_prime_sequence expected_j_prime;
	struct v34_phase4_j_prime_sequence rejected_j_prime;
	struct v34_phase4_j_prime_sequence saved_j_prime;
	unsigned int extra_trn =
		constellation == V34_CONSTELLATION_16_POINT ? 37u : 0u;

	CHECK(v34_phase3_transmitter_init(
		      &transmitter, role, constellation) ==
		      V34_PHASE3_TRANSMITTER_OK &&
	      v34_phase3_s_sequence_init(&expected_s) ==
		      V34_PHASE3_S_SEQUENCE_OK &&
	      v34_phase3_transmitter_is_valid(&transmitter) &&
	      transmitter.local_role == role &&
	      transmitter.stage == V34_PHASE3_TRANSMITTER_S,
	      "Phase 3 transmitter did not initialize role=%d constellation=%d",
	      role, constellation);
	for (unsigned int symbol = 0;
	     symbol < V34_PHASE3_S_SEQUENCE_SYMBOLS; ++symbol) {
		struct qam_point actual;
		struct qam_point expected;
		enum v34_phase3_s_section section;

		CHECK(v34_phase3_s_sequence_next(
			      &expected_s, &expected, &section) ==
			      V34_PHASE3_S_SEQUENCE_OK &&
		      v34_phase3_transmitter_next(&transmitter, &actual) ==
			      V34_PHASE3_TRANSMITTER_OK &&
		      points_match(&actual, &expected),
		      "S/S-bar composition diverged at symbol %u", symbol);
	}
	CHECK(v34_phase3_s_sequence_is_complete(&expected_s) &&
	      transmitter.stage == V34_PHASE3_TRANSMITTER_PP &&
	      v34_phase3_pp_sequence_init(&expected_pp) ==
		      V34_PHASE3_PP_SEQUENCE_OK,
	      "S/S-bar did not hand off to PP");
	for (unsigned int symbol = 0;
	     symbol < V34_PHASE3_PP_SEQUENCE_SYMBOLS; ++symbol) {
		struct qam_point actual;
		struct qam_point expected;

		CHECK(v34_phase3_pp_sequence_next(&expected_pp, &expected) ==
			      V34_PHASE3_PP_SEQUENCE_OK &&
		      v34_phase3_transmitter_next(&transmitter, &actual) ==
			      V34_PHASE3_TRANSMITTER_OK &&
		      points_match(&actual, &expected),
		      "PP composition diverged at symbol %u", symbol);
	}
	CHECK(v34_phase3_pp_sequence_is_complete(&expected_pp) &&
	      transmitter.stage == V34_PHASE3_TRANSMITTER_TRN &&
	      v34_training_sequence_init(
		      &expected_trn, role, V34_CONSTELLATION_4_POINT) ==
		      V34_TRAINING_SEQUENCE_OK,
	      "PP did not hand off to TRN");
	for (unsigned int symbol = 0;
	     symbol < V34_TRAINING_SEQUENCE_MIN_SYMBOLS + extra_trn; ++symbol) {
		struct qam_point actual;
		struct qam_point expected;
		uint8_t quarter;
		uint8_t rotation;

		if (symbol + 1u == V34_TRAINING_SEQUENCE_MIN_SYMBOLS) {
			struct v34_phase3_transmitter saved = transmitter;

			CHECK(v34_phase3_transmitter_start_j(&transmitter) ==
				      V34_PHASE3_TRANSMITTER_INVALID_STATE &&
			      memcmp(&transmitter, &saved, sizeof(transmitter)) == 0,
			      "TRN ended before its minimum");
		}
		CHECK(v34_training_sequence_next(
			      &expected_trn, &expected, &quarter, &rotation) ==
			      V34_TRAINING_SEQUENCE_OK &&
		      v34_phase3_transmitter_next(&transmitter, &actual) ==
			      V34_PHASE3_TRANSMITTER_OK &&
		      points_match(&actual, &expected),
		      "TRN composition diverged at symbol %u", symbol);
	}
	CHECK(transmitter.trn_symbols ==
		      V34_TRAINING_SEQUENCE_MIN_SYMBOLS + extra_trn &&
	      trn_matches(&transmitter.state.trn, &expected_trn) &&
	      v34_phase3_transmitter_start_j(&transmitter) ==
		      V34_PHASE3_TRANSMITTER_OK &&
	      v34_phase3_j_sequence_init_from_trn(
		      &expected_j, &expected_trn, constellation) ==
		      V34_PHASE3_J_SEQUENCE_OK &&
	      j_matches(&transmitter.state.j, &expected_j),
	      "TRN did not preserve its J boundary");
	memset(&rejected_j_prime, 0xa5, sizeof(rejected_j_prime));
	saved_j_prime = rejected_j_prime;
	CHECK(v34_phase3_transmitter_finish_j(&transmitter) ==
		      V34_PHASE3_TRANSMITTER_INVALID_STATE &&
	      v34_phase3_transmitter_start_j_prime(
		      &transmitter, &rejected_j_prime) ==
		      V34_PHASE3_TRANSMITTER_INVALID_STATE &&
	      memcmp(&rejected_j_prime, &saved_j_prime,
		     sizeof(rejected_j_prime)) == 0,
	      "empty J interval crossed the Phase 4 boundary");
	for (unsigned int symbol = 0;
	     symbol < V34_PHASE3_J_PATTERN_SYMBOLS; ++symbol) {
		struct qam_point actual;
		struct qam_point expected;
		uint8_t rotation;

		CHECK(v34_phase3_j_sequence_next(
			      &expected_j, &expected, &rotation) ==
			      V34_PHASE3_J_SEQUENCE_OK &&
		      v34_phase3_transmitter_next(&transmitter, &actual) ==
			      V34_PHASE3_TRANSMITTER_OK &&
		      points_match(&actual, &expected),
		      "J composition diverged at symbol %u", symbol);
		if (symbol == 0) {
			struct v34_phase3_transmitter saved = transmitter;

			CHECK(v34_phase3_transmitter_finish_j(&transmitter) ==
				      V34_PHASE3_TRANSMITTER_INVALID_STATE &&
			      memcmp(&transmitter, &saved, sizeof(transmitter)) == 0,
			      "J ended between pattern boundaries");
		}
	}
	CHECK(v34_phase3_j_sequence_at_pattern_boundary(&expected_j) &&
	      v34_phase3_transmitter_finish_j(&transmitter) ==
		      V34_PHASE3_TRANSMITTER_OK &&
	      transmitter.stage == V34_PHASE3_TRANSMITTER_COMPLETE,
	      "J did not reach the Phase 3 boundary");
	if (role == V34_CALLING_STATION) {
		CHECK(v34_phase3_transmitter_start_j_prime(
			      &transmitter, &j_prime) ==
			      V34_PHASE3_TRANSMITTER_OK &&
		      v34_phase4_j_prime_sequence_init_from_j(
			      &expected_j_prime, &expected_j) ==
			      V34_PHASE4_J_PRIME_SEQUENCE_OK,
		      "calling J did not hand off to J-prime");
		for (unsigned int symbol = 0;
		     symbol < V34_PHASE4_J_PRIME_SYMBOLS; ++symbol) {
			struct qam_point actual;
			struct qam_point expected;
			uint8_t actual_rotation;
			uint8_t expected_rotation;

			CHECK(v34_phase4_j_prime_sequence_next(
				      &j_prime, &actual, &actual_rotation) ==
				      V34_PHASE4_J_PRIME_SEQUENCE_OK &&
			      v34_phase4_j_prime_sequence_next(
				      &expected_j_prime, &expected,
				      &expected_rotation) ==
				      V34_PHASE4_J_PRIME_SEQUENCE_OK &&
			      points_match(&actual, &expected) &&
			      actual_rotation == expected_rotation,
			      "J-prime handoff diverged at symbol %u", symbol);
		}
	} else {
		memset(&j_prime, 0xa5, sizeof(j_prime));
		saved_j_prime = j_prime;
		CHECK(v34_phase3_transmitter_start_j_prime(
			      &transmitter, &j_prime) ==
			      V34_PHASE3_TRANSMITTER_INVALID_STATE &&
		      memcmp(&j_prime, &saved_j_prime,
			     sizeof(j_prime)) == 0,
		      "answering J crossed the caller-only J-prime boundary");
	}
	{
		struct v34_phase3_transmitter saved = transmitter;
		struct qam_point output = { 12.0, -7.0 };
		struct qam_point expected_output = output;

		CHECK(v34_phase3_transmitter_next(&transmitter, &output) ==
			      V34_PHASE3_TRANSMITTER_DONE &&
		      memcmp(&transmitter, &saved, sizeof(transmitter)) == 0 &&
		      memcmp(&output, &expected_output, sizeof(output)) == 0,
		      "completed Phase 3 transmitter emitted another symbol");
	}
	return true;
}

static bool test_sequences(void)
{
	return run_sequence(
		       V34_CALLING_STATION,
		       V34_CONSTELLATION_4_POINT) &&
	       run_sequence(
		       V34_CALLING_STATION,
		       V34_CONSTELLATION_16_POINT) &&
	       run_sequence(
		       V34_ANSWERING_STATION,
		       V34_CONSTELLATION_4_POINT) &&
	       run_sequence(
		       V34_ANSWERING_STATION,
		       V34_CONSTELLATION_16_POINT);
}

static bool test_validation_and_overflow(void)
{
	struct v34_phase3_transmitter transmitter;
	struct v34_phase3_transmitter saved;
	struct qam_point point = { 3.0, 4.0 };
	struct qam_point saved_point = point;

	memset(&transmitter, 0xa5, sizeof(transmitter));
	saved = transmitter;
	CHECK(v34_phase3_transmitter_init(
		      NULL, V34_CALLING_STATION,
		      V34_CONSTELLATION_4_POINT) ==
		      V34_PHASE3_TRANSMITTER_INVALID_ARGUMENT &&
	      v34_phase3_transmitter_init(
		      &transmitter, V34_CALLING_STATION,
		      (enum v34_constellation_size)99) ==
		      V34_PHASE3_TRANSMITTER_INVALID_ARGUMENT &&
	      v34_phase3_transmitter_init(
		      &transmitter, (enum v34_station_role)99,
		      V34_CONSTELLATION_4_POINT) ==
		      V34_PHASE3_TRANSMITTER_INVALID_ARGUMENT &&
	      memcmp(&transmitter, &saved, sizeof(transmitter)) == 0 &&
	      !v34_phase3_transmitter_is_valid(NULL),
	      "invalid Phase 3 initialization changed its destination");
	CHECK(v34_phase3_transmitter_init(
		      &transmitter, V34_CALLING_STATION,
		      V34_CONSTELLATION_4_POINT) ==
		      V34_PHASE3_TRANSMITTER_OK,
	      "validation transmitter did not initialize");
	saved = transmitter;
	CHECK(v34_phase3_transmitter_next(&transmitter, NULL) ==
		      V34_PHASE3_TRANSMITTER_INVALID_ARGUMENT &&
	      memcmp(&transmitter, &saved, sizeof(transmitter)) == 0,
	      "null output changed the Phase 3 transmitter");
	transmitter.local_role = (enum v34_station_role)99;
	saved = transmitter;
	CHECK(!v34_phase3_transmitter_is_valid(&transmitter) &&
	      v34_phase3_transmitter_next(&transmitter, &point) ==
		      V34_PHASE3_TRANSMITTER_INVALID_ARGUMENT &&
	      memcmp(&transmitter, &saved, sizeof(transmitter)) == 0 &&
	      memcmp(&point, &saved_point, sizeof(point)) == 0,
	      "invalid Phase 3 role changed its output");
	CHECK(v34_phase3_transmitter_init(
		      &transmitter, V34_CALLING_STATION,
		      V34_CONSTELLATION_4_POINT) ==
		      V34_PHASE3_TRANSMITTER_OK,
	      "stage validation transmitter did not initialize");
	transmitter.stage = (enum v34_phase3_transmitter_stage)99;
	saved = transmitter;
	CHECK(!v34_phase3_transmitter_is_valid(&transmitter) &&
	      v34_phase3_transmitter_next(&transmitter, &point) ==
		      V34_PHASE3_TRANSMITTER_INVALID_ARGUMENT &&
	      memcmp(&transmitter, &saved, sizeof(transmitter)) == 0 &&
	      memcmp(&point, &saved_point, sizeof(point)) == 0,
	      "invalid Phase 3 state changed its output");
	CHECK(v34_phase3_transmitter_init(
		      &transmitter, V34_CALLING_STATION,
		      V34_CONSTELLATION_4_POINT) ==
		      V34_PHASE3_TRANSMITTER_OK,
	      "overflow transmitter did not initialize");
	for (size_t symbol = 0;
	     symbol < V34_PHASE3_S_SEQUENCE_SYMBOLS +
			      V34_PHASE3_PP_SEQUENCE_SYMBOLS + 1u;
	     ++symbol)
		CHECK(v34_phase3_transmitter_next(&transmitter, &point) ==
			      V34_PHASE3_TRANSMITTER_OK,
		      "overflow transmitter did not reach TRN");
	transmitter.trn_symbols = UINT32_MAX;
	saved = transmitter;
	point = saved_point;
	CHECK(v34_phase3_transmitter_is_valid(&transmitter) &&
	      v34_phase3_transmitter_next(&transmitter, &point) ==
		      V34_PHASE3_TRANSMITTER_NUMERIC_FAILURE &&
	      memcmp(&transmitter, &saved, sizeof(transmitter)) == 0 &&
	      memcmp(&point, &saved_point, sizeof(point)) == 0,
	      "TRN counter overflow was not transactional");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "composed role sequences", test_sequences },
		{ "validation and overflow", test_validation_and_overflow },
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
