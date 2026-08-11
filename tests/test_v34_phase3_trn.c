// SPDX-License-Identifier: BSD-3-Clause

#include <v34_phase3_trn.h>

#include "test.h"

#include <math.h>
#include <string.h>

enum {
	REFERENCE_HISTORY_BITS = 23,
	ROLE_TEST_SYMBOLS = 2048,
	CALLING_PREFIX_SYMBOLS = 64,
};

static const double TEST_PI =
	3.1415926535897932384626433832795;

struct reference_scrambler {
	uint8_t history[REFERENCE_HISTORY_BITS];
	unsigned int first_tap_delay;
};

static uint8_t reference_scrambled_one(
	struct reference_scrambler *scrambler)
{
	uint8_t feedback =
		scrambler->history[scrambler->first_tap_delay - 1u] ^
		scrambler->history[REFERENCE_HISTORY_BITS - 1u];
	uint8_t output = 1u ^ feedback;

	for (size_t delay = REFERENCE_HISTORY_BITS - 1u; delay > 0; --delay)
		scrambler->history[delay] = scrambler->history[delay - 1u];
	scrambler->history[0] = output;
	return output;
}

static uint8_t reference_rotation(struct reference_scrambler *scrambler)
{
	uint8_t i1 = reference_scrambled_one(scrambler);
	uint8_t i2 = reference_scrambled_one(scrambler);

	return (uint8_t)((i2 << 1) | i1);
}

static uint32_t reference_history(
	const struct reference_scrambler *scrambler)
{
	uint32_t history = 0;

	for (size_t delay = 0; delay < REFERENCE_HISTORY_BITS; ++delay)
		history |= (uint32_t)scrambler->history[delay] << delay;
	return history;
}

static bool test_role_sequences(void)
{
	static const enum v34_station_role roles[] = {
		V34_CALLING_STATION,
		V34_ANSWERING_STATION,
	};
	uint8_t calling_prefix[CALLING_PREFIX_SYMBOLS];
	bool roles_diverged = false;

	CHECK(V34_PHASE3_TRN_MIN_SYMBOLS == 512,
	      "Phase 3 TRN minimum changed");
	for (size_t role_index = 0; role_index < ARRAY_LENGTH(roles);
	     ++role_index) {
		enum v34_station_role role = roles[role_index];
		struct v34_phase3_trn trn;
		struct reference_scrambler reference = {
			.first_tap_delay =
				role == V34_CALLING_STATION ? 18u : 5u,
		};
		unsigned int rotation_counts[4] = { 0 };
		bool asymmetric_pair_seen = false;

		CHECK(v34_phase3_trn_init(&trn, role) ==
			      V34_PHASE3_TRN_OK &&
		      v34_phase3_trn_is_valid(&trn) &&
		      trn.scrambler.history == 0 && !trn.has_last_symbol &&
		      trn.last_clockwise_quarter_turns == 0,
		      "TRN role %zu did not initialize", role_index);
		for (unsigned int symbol = 0; symbol < ROLE_TEST_SYMBOLS;
		     ++symbol) {
			struct qam_point point = { 7.0, -9.0 };
			uint8_t rotation = UINT8_MAX;
			uint8_t expected = reference_rotation(&reference);
			double angle = TEST_PI / 4.0 - expected * TEST_PI / 2.0;
			double power;

			CHECK(v34_phase3_trn_next(
				      &trn, &point, &rotation) ==
				      V34_PHASE3_TRN_OK,
			      "TRN role %zu stopped at symbol %u",
			      role_index, symbol);
			power = point.in_phase * point.in_phase +
				point.quadrature * point.quadrature;
			CHECK(rotation == expected &&
			      trn.last_clockwise_quarter_turns == expected &&
			      trn.has_last_symbol &&
			      fabs(point.in_phase - cos(angle)) < 1e-14 &&
			      fabs(point.quadrature - sin(angle)) < 1e-14 &&
			      fabs(power - 1.0) < 1e-14 &&
			      v34_phase3_trn_is_valid(&trn),
			      "TRN role %zu symbol %u changed", role_index,
			      symbol);
			rotation_counts[rotation]++;
			if (rotation == 1u || rotation == 2u)
				asymmetric_pair_seen = true;
			if (symbol < CALLING_PREFIX_SYMBOLS) {
				if (role == V34_CALLING_STATION)
					calling_prefix[symbol] = rotation;
				else if (calling_prefix[symbol] != rotation)
					roles_diverged = true;
			}
		}
		CHECK(trn.scrambler.history == reference_history(&reference) &&
		      asymmetric_pair_seen && rotation_counts[0] > 0 &&
		      rotation_counts[1] > 0 && rotation_counts[2] > 0 &&
		      rotation_counts[3] > 0,
		      "TRN role %zu lost its polynomial or symbol coverage",
		      role_index);
	}
	CHECK(roles_diverged,
	      "calling and answering TRN polynomials did not diverge");
	return true;
}

static bool test_copy_and_restart(void)
{
	struct v34_phase3_trn first;
	struct v34_phase3_trn second;
	struct v34_phase3_trn restarted;
	struct qam_point ignored;
	uint8_t ignored_rotation;

	CHECK(v34_phase3_trn_init(
		      &first, V34_CALLING_STATION) ==
		      V34_PHASE3_TRN_OK,
	      "TRN copy fixture did not initialize");
	for (unsigned int symbol = 0; symbol < 77; ++symbol)
		CHECK(v34_phase3_trn_next(
			      &first, &ignored, &ignored_rotation) ==
			      V34_PHASE3_TRN_OK,
		      "TRN prefix stopped at symbol %u", symbol);
	second = first;
	for (unsigned int symbol = 0; symbol < 1024; ++symbol) {
		struct qam_point first_point;
		struct qam_point second_point;
		uint8_t first_rotation;
		uint8_t second_rotation;

		CHECK(v34_phase3_trn_next(
			      &first, &first_point, &first_rotation) ==
			      V34_PHASE3_TRN_OK &&
		      v34_phase3_trn_next(
			      &second, &second_point, &second_rotation) ==
			      V34_PHASE3_TRN_OK &&
		      first_rotation == second_rotation &&
		      memcmp(&first_point, &second_point,
			     sizeof(first_point)) == 0 &&
		      memcmp(&first, &second, sizeof(first)) == 0,
		      "copied TRN stream diverged at symbol %u", symbol);
	}
	CHECK(v34_phase3_trn_init(
		      &restarted, V34_CALLING_STATION) ==
		      V34_PHASE3_TRN_OK &&
	      v34_phase3_trn_next(
		      &restarted, &ignored, &ignored_rotation) ==
		      V34_PHASE3_TRN_OK &&
	      ignored_rotation == 3u,
	      "restarted TRN did not return to the all-zero scrambler state");
	return true;
}

static bool test_validation(void)
{
	struct v34_phase3_trn trn;
	struct v34_phase3_trn before;
	struct qam_point point = { 7.0, -9.0 };
	struct qam_point before_point = point;
	uint8_t rotation = UINT8_MAX;

	memset(&trn, 0xa5, sizeof(trn));
	before = trn;
	CHECK(v34_phase3_trn_init(
		      NULL, V34_CALLING_STATION) ==
		      V34_PHASE3_TRN_INVALID_ARGUMENT &&
	      v34_phase3_trn_init(
		      &trn, (enum v34_station_role)99) ==
		      V34_PHASE3_TRN_INVALID_ARGUMENT &&
	      memcmp(&trn, &before, sizeof(trn)) == 0 &&
	      !v34_phase3_trn_is_valid(NULL),
	      "invalid TRN initialization mutated its destination");
	CHECK(v34_phase3_trn_init(
		      &trn, V34_ANSWERING_STATION) ==
		      V34_PHASE3_TRN_OK,
	      "TRN validation fixture did not initialize");
	before = trn;
	CHECK(v34_phase3_trn_next(
		      &trn, NULL, &rotation) ==
		      V34_PHASE3_TRN_INVALID_ARGUMENT &&
	      v34_phase3_trn_next(&trn, &point, NULL) ==
		      V34_PHASE3_TRN_INVALID_ARGUMENT &&
	      memcmp(&trn, &before, sizeof(trn)) == 0 &&
	      memcmp(&point, &before_point, sizeof(point)) == 0 &&
	      rotation == UINT8_MAX,
	      "invalid TRN output argument mutated state");
	trn.scrambler.history = UINT32_C(1) << 23;
	before = trn;
	CHECK(!v34_phase3_trn_is_valid(&trn) &&
	      v34_phase3_trn_next(&trn, &point, &rotation) ==
		      V34_PHASE3_TRN_INVALID_ARGUMENT &&
	      memcmp(&trn, &before, sizeof(trn)) == 0 &&
	      memcmp(&point, &before_point, sizeof(point)) == 0 &&
	      rotation == UINT8_MAX,
	      "invalid TRN scrambler state mutated outputs");
	trn.scrambler.history = 0;
	trn.last_clockwise_quarter_turns = 4;
	CHECK(!v34_phase3_trn_is_valid(&trn),
	      "TRN accepted an invalid last rotation");
	trn.last_clockwise_quarter_turns = 1;
	trn.has_last_symbol = false;
	CHECK(!v34_phase3_trn_is_valid(&trn),
	      "TRN accepted a last rotation without a symbol");
	CHECK(strcmp(v34_phase3_trn_result_name(
			     V34_PHASE3_TRN_INVALID_ARGUMENT),
		     "invalid argument") == 0 &&
	      strcmp(v34_phase3_trn_result_name(
			     (enum v34_phase3_trn_result)99),
		     "unknown") == 0,
	      "TRN result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "role sequences", test_role_sequences },
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
