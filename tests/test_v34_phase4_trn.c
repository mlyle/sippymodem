// SPDX-License-Identifier: BSD-3-Clause

#include <v34_phase4_trn.h>

#include "test.h"

#include <math.h>
#include <string.h>

enum {
	REFERENCE_HISTORY_BITS = 23,
	ROLE_SYMBOLS = 2048,
	PREFIX_SYMBOLS = 64,
};

static const double TEST_TOLERANCE = 1e-14;

struct reference_scrambler {
	uint8_t history[REFERENCE_HISTORY_BITS];
	unsigned int first_tap_delay;
};

static void reference_scrambler_init(
	struct reference_scrambler *scrambler,
	enum sippymodem_v34_station_role role)
{
	memset(scrambler, 0, sizeof(*scrambler));
	scrambler->first_tap_delay =
		role == SIPPYMODEM_V34_CALLING_STATION ? 18u : 5u;
}

static uint8_t reference_scrambled_one(
	struct reference_scrambler *scrambler)
{
	uint8_t output = 1u ^
		scrambler->history[scrambler->first_tap_delay - 1u] ^
		scrambler->history[REFERENCE_HISTORY_BITS - 1u];

	for (size_t delay = REFERENCE_HISTORY_BITS - 1u; delay > 0; --delay)
		scrambler->history[delay] = scrambler->history[delay - 1u];
	scrambler->history[0] = output;
	return output;
}

static uint8_t reference_mapped_dibit(
	struct reference_scrambler *scrambler)
{
	uint8_t first = reference_scrambled_one(scrambler);
	uint8_t second = reference_scrambled_one(scrambler);

	return (uint8_t)((second << 1) | first);
}

static uint32_t reference_history(
	const struct reference_scrambler *scrambler)
{
	uint32_t history = 0;

	for (size_t delay = 0; delay < REFERENCE_HISTORY_BITS; ++delay)
		history |= (uint32_t)scrambler->history[delay] << delay;
	return history;
}

static struct sippymodem_qam_point reference_point(
	enum sippymodem_v34_constellation_size constellation,
	uint8_t quarter_point, uint8_t clockwise_quarter_turns)
{
	double grid_unit =
		constellation == SIPPYMODEM_V34_CONSTELLATION_4_POINT ?
			sqrt(0.5) : sqrt(0.1);
	double in_phase = (quarter_point & 1u) != 0u ? -3.0 : 1.0;
	double quadrature = (quarter_point & 2u) != 0u ? -3.0 : 1.0;
	double saved_in_phase;

	for (uint8_t turn = 0; turn < clockwise_quarter_turns; ++turn) {
		saved_in_phase = in_phase;
		in_phase = quadrature;
		quadrature = -saved_in_phase;
	}
	return (struct sippymodem_qam_point){
		.in_phase = in_phase * grid_unit,
		.quadrature = quadrature * grid_unit,
	};
}

static bool points_are_close(
	const struct sippymodem_qam_point *left,
	const struct sippymodem_qam_point *right)
{
	return fabs(left->in_phase - right->in_phase) < TEST_TOLERANCE &&
	       fabs(left->quadrature - right->quadrature) < TEST_TOLERANCE;
}

static bool test_role_and_constellation_sequences(void)
{
	static const enum sippymodem_v34_station_role roles[] = {
		SIPPYMODEM_V34_CALLING_STATION,
		SIPPYMODEM_V34_ANSWERING_STATION,
	};
	static const enum sippymodem_v34_constellation_size constellations[] = {
		SIPPYMODEM_V34_CONSTELLATION_4_POINT,
		SIPPYMODEM_V34_CONSTELLATION_16_POINT,
	};
	uint8_t calling_prefix[ARRAY_LENGTH(constellations)][PREFIX_SYMBOLS][2];
	bool roles_diverged[ARRAY_LENGTH(constellations)] = { false };

	CHECK(SIPPYMODEM_V34_PHASE4_TRN_MIN_SYMBOLS == 512,
	      "Phase 4 TRN minimum changed");
	for (size_t role_index = 0; role_index < ARRAY_LENGTH(roles);
	     ++role_index) {
		for (size_t constellation_index = 0;
		     constellation_index < ARRAY_LENGTH(constellations);
		     ++constellation_index) {
			enum sippymodem_v34_station_role role = roles[role_index];
			enum sippymodem_v34_constellation_size constellation =
				constellations[constellation_index];
			struct sippymodem_v34_phase4_trn trn;
			struct reference_scrambler reference;
			unsigned int rotation_counts[4] = { 0 };
			unsigned int quarter_counts[4] = { 0 };

			reference_scrambler_init(&reference, role);
			CHECK(sippymodem_v34_phase4_trn_init(
				      &trn, role, constellation) ==
				      SIPPYMODEM_V34_PHASE4_TRN_OK &&
			      sippymodem_v34_phase4_trn_is_valid(&trn) &&
			      trn.scrambler.history == 0 &&
			      trn.role == role &&
			      trn.constellation == constellation &&
			      !trn.has_last_symbol &&
			      trn.last_clockwise_quarter_turns == 0,
			      "TRN selection %zu/%zu did not initialize",
			      role_index, constellation_index);
			for (unsigned int symbol = 0; symbol < ROLE_SYMBOLS;
			     ++symbol) {
				uint8_t expected_rotation =
					reference_mapped_dibit(&reference);
				uint8_t expected_quarter =
					constellation ==
							SIPPYMODEM_V34_CONSTELLATION_16_POINT ?
						reference_mapped_dibit(&reference) : 0u;
				struct sippymodem_qam_point expected = reference_point(
					constellation, expected_quarter,
					expected_rotation);
				struct sippymodem_qam_point point = { 7.0, -9.0 };
				uint8_t quarter = UINT8_MAX;
				uint8_t rotation = UINT8_MAX;

				CHECK(sippymodem_v34_phase4_trn_next(
					      &trn, &point, &quarter, &rotation) ==
					      SIPPYMODEM_V34_PHASE4_TRN_OK,
				      "TRN selection %zu/%zu stopped at %u",
				      role_index, constellation_index, symbol);
				CHECK(rotation == expected_rotation &&
				      quarter == expected_quarter &&
				      trn.last_clockwise_quarter_turns ==
					      expected_rotation &&
				      trn.has_last_symbol &&
				      points_are_close(&point, &expected) &&
				      sippymodem_v34_phase4_trn_is_valid(&trn),
				      "TRN selection %zu/%zu symbol %u changed",
				      role_index, constellation_index, symbol);
				if (rotation < ARRAY_LENGTH(rotation_counts) &&
				    quarter < ARRAY_LENGTH(quarter_counts)) {
					rotation_counts[rotation]++;
					quarter_counts[quarter]++;
				} else {
					CHECK(false,
					      "TRN selection %zu/%zu emitted an invalid index",
					      role_index, constellation_index);
				}
				if (symbol < PREFIX_SYMBOLS) {
					if (role == SIPPYMODEM_V34_CALLING_STATION) {
						calling_prefix[constellation_index]
							      [symbol][0] = rotation;
						calling_prefix[constellation_index]
							      [symbol][1] = quarter;
					} else if (calling_prefix[constellation_index]
							 [symbol][0] != rotation ||
						   calling_prefix[constellation_index]
							 [symbol][1] != quarter) {
						roles_diverged[constellation_index] = true;
					}
				}
			}
			CHECK(trn.scrambler.history ==
				      reference_history(&reference) &&
			      rotation_counts[0] > 0 && rotation_counts[1] > 0 &&
			      rotation_counts[2] > 0 && rotation_counts[3] > 0 &&
			      quarter_counts[0] > 0 &&
			      (constellation ==
				       SIPPYMODEM_V34_CONSTELLATION_4_POINT ||
			       (quarter_counts[1] > 0 && quarter_counts[2] > 0 &&
				quarter_counts[3] > 0)),
			      "TRN selection %zu/%zu lost state coverage",
			      role_index, constellation_index);
		}
	}
	CHECK(roles_diverged[0] && roles_diverged[1],
	      "station-role TRN polynomials did not diverge");
	return true;
}

static bool test_copy_and_restart(void)
{
	static const enum sippymodem_v34_constellation_size constellations[] = {
		SIPPYMODEM_V34_CONSTELLATION_4_POINT,
		SIPPYMODEM_V34_CONSTELLATION_16_POINT,
	};

	for (size_t selection = 0; selection < ARRAY_LENGTH(constellations);
	     ++selection) {
		struct sippymodem_v34_phase4_trn first;
		struct sippymodem_v34_phase4_trn second;
		struct sippymodem_v34_phase4_trn restarted;
		struct sippymodem_qam_point ignored_point;
		uint8_t ignored_quarter;
		uint8_t ignored_rotation;

		CHECK(sippymodem_v34_phase4_trn_init(
			      &first, SIPPYMODEM_V34_ANSWERING_STATION,
			      constellations[selection]) ==
			      SIPPYMODEM_V34_PHASE4_TRN_OK,
		      "TRN copy selection %zu did not initialize", selection);
		for (unsigned int symbol = 0; symbol < 73; ++symbol)
			CHECK(sippymodem_v34_phase4_trn_next(
				      &first, &ignored_point, &ignored_quarter,
				      &ignored_rotation) ==
				      SIPPYMODEM_V34_PHASE4_TRN_OK,
			      "TRN copy prefix stopped at %zu/%u", selection,
			      symbol);
		second = first;
		for (unsigned int symbol = 0; symbol < 1024; ++symbol) {
			struct sippymodem_qam_point first_point;
			struct sippymodem_qam_point second_point;
			uint8_t first_quarter;
			uint8_t second_quarter;
			uint8_t first_rotation;
			uint8_t second_rotation;

			CHECK(sippymodem_v34_phase4_trn_next(
				      &first, &first_point, &first_quarter,
				      &first_rotation) ==
				      SIPPYMODEM_V34_PHASE4_TRN_OK &&
			      sippymodem_v34_phase4_trn_next(
				      &second, &second_point, &second_quarter,
				      &second_rotation) ==
				      SIPPYMODEM_V34_PHASE4_TRN_OK &&
			      first_quarter == second_quarter &&
			      first_rotation == second_rotation &&
			      memcmp(&first_point, &second_point,
				     sizeof(first_point)) == 0 &&
			      memcmp(&first, &second, sizeof(first)) == 0,
			      "copied TRN stream diverged at %zu/%u", selection,
			      symbol);
		}
		CHECK(sippymodem_v34_phase4_trn_init(
			      &restarted, SIPPYMODEM_V34_ANSWERING_STATION,
			      constellations[selection]) ==
			      SIPPYMODEM_V34_PHASE4_TRN_OK &&
		      restarted.scrambler.history == 0 &&
		      !restarted.has_last_symbol,
		      "restarted TRN selection %zu retained state", selection);
	}
	return true;
}

static bool test_validation(void)
{
	struct sippymodem_v34_phase4_trn trn;
	struct sippymodem_v34_phase4_trn before;
	struct sippymodem_qam_point point = { 7.0, -9.0 };
	struct sippymodem_qam_point before_point = point;
	uint8_t quarter = UINT8_MAX;
	uint8_t rotation = UINT8_MAX;

	memset(&trn, 0xa5, sizeof(trn));
	before = trn;
	CHECK(sippymodem_v34_phase4_trn_init(
		      NULL, SIPPYMODEM_V34_CALLING_STATION,
		      SIPPYMODEM_V34_CONSTELLATION_4_POINT) ==
		      SIPPYMODEM_V34_PHASE4_TRN_INVALID_ARGUMENT &&
	      sippymodem_v34_phase4_trn_init(
		      &trn, (enum sippymodem_v34_station_role)99,
		      SIPPYMODEM_V34_CONSTELLATION_4_POINT) ==
		      SIPPYMODEM_V34_PHASE4_TRN_INVALID_ARGUMENT &&
	      sippymodem_v34_phase4_trn_init(
		      &trn, SIPPYMODEM_V34_CALLING_STATION,
		      (enum sippymodem_v34_constellation_size)8) ==
		      SIPPYMODEM_V34_PHASE4_TRN_INVALID_ARGUMENT &&
	      memcmp(&trn, &before, sizeof(trn)) == 0 &&
	      !sippymodem_v34_phase4_trn_is_valid(NULL),
	      "invalid TRN initialization mutated its destination");
	CHECK(sippymodem_v34_phase4_trn_init(
		      &trn, SIPPYMODEM_V34_CALLING_STATION,
		      SIPPYMODEM_V34_CONSTELLATION_16_POINT) ==
		      SIPPYMODEM_V34_PHASE4_TRN_OK,
	      "TRN validation fixture did not initialize");
	before = trn;
	CHECK(sippymodem_v34_phase4_trn_next(
		      NULL, &point, &quarter, &rotation) ==
		      SIPPYMODEM_V34_PHASE4_TRN_INVALID_ARGUMENT &&
	      sippymodem_v34_phase4_trn_next(
		      &trn, NULL, &quarter, &rotation) ==
		      SIPPYMODEM_V34_PHASE4_TRN_INVALID_ARGUMENT &&
	      sippymodem_v34_phase4_trn_next(
		      &trn, &point, NULL, &rotation) ==
		      SIPPYMODEM_V34_PHASE4_TRN_INVALID_ARGUMENT &&
	      sippymodem_v34_phase4_trn_next(
		      &trn, &point, &quarter, NULL) ==
		      SIPPYMODEM_V34_PHASE4_TRN_INVALID_ARGUMENT &&
	      memcmp(&trn, &before, sizeof(trn)) == 0 &&
	      memcmp(&point, &before_point, sizeof(point)) == 0 &&
	      quarter == UINT8_MAX && rotation == UINT8_MAX,
	      "invalid TRN output argument mutated state");
	trn.scrambler.history = UINT32_C(1) << SIPPYMODEM_V34_SCRAMBLER_BITS;
	before = trn;
	CHECK(!sippymodem_v34_phase4_trn_is_valid(&trn) &&
	      sippymodem_v34_phase4_trn_next(
		      &trn, &point, &quarter, &rotation) ==
		      SIPPYMODEM_V34_PHASE4_TRN_INVALID_ARGUMENT &&
	      memcmp(&trn, &before, sizeof(trn)) == 0 &&
	      memcmp(&point, &before_point, sizeof(point)) == 0 &&
	      quarter == UINT8_MAX && rotation == UINT8_MAX,
	      "corrupt TRN state mutated outputs");
	trn = (struct sippymodem_v34_phase4_trn){ 0 };
	CHECK(sippymodem_v34_phase4_trn_init(
		      &trn, SIPPYMODEM_V34_CALLING_STATION,
		      SIPPYMODEM_V34_CONSTELLATION_4_POINT) ==
		      SIPPYMODEM_V34_PHASE4_TRN_OK,
	      "TRN corruption fixture did not initialize");
	trn.scrambler.polynomial = SIPPYMODEM_V34_POLYNOMIAL_GPA;
	CHECK(!sippymodem_v34_phase4_trn_is_valid(&trn),
	      "TRN accepted the other station's polynomial");
	trn.scrambler.polynomial = SIPPYMODEM_V34_POLYNOMIAL_GPC;
	trn.role = (enum sippymodem_v34_station_role)99;
	CHECK(!sippymodem_v34_phase4_trn_is_valid(&trn),
	      "TRN accepted an invalid role");
	trn.role = SIPPYMODEM_V34_CALLING_STATION;
	trn.constellation = (enum sippymodem_v34_constellation_size)8;
	CHECK(!sippymodem_v34_phase4_trn_is_valid(&trn),
	      "TRN accepted an invalid constellation");
	trn.constellation = SIPPYMODEM_V34_CONSTELLATION_4_POINT;
	trn.last_clockwise_quarter_turns = 4;
	CHECK(!sippymodem_v34_phase4_trn_is_valid(&trn),
	      "TRN accepted an invalid final rotation");
	trn.last_clockwise_quarter_turns = 1;
	trn.has_last_symbol = false;
	CHECK(!sippymodem_v34_phase4_trn_is_valid(&trn),
	      "TRN accepted a final rotation without a symbol");
	CHECK(strcmp(sippymodem_v34_phase4_trn_result_name(
			     SIPPYMODEM_V34_PHASE4_TRN_INVALID_ARGUMENT),
		     "invalid argument") == 0 &&
	      strcmp(sippymodem_v34_phase4_trn_result_name(
			     (enum sippymodem_v34_phase4_trn_result)99),
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
		{ "role and constellation sequences",
		  test_role_and_constellation_sequences },
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
