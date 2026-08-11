// SPDX-License-Identifier: BSD-3-Clause

#include <v34_phase3_s_sequence.h>

#include "test.h"

#include <math.h>
#include <string.h>

static unsigned int expected_quarter_turns(unsigned int symbol)
{
	if (symbol < V34_PHASE3_S_SYMBOLS)
		return symbol & 1u;
	return 2u + ((symbol - V34_PHASE3_S_SYMBOLS) & 1u);
}

static enum v34_phase3_s_section expected_section(
	unsigned int symbol)
{
	return symbol < V34_PHASE3_S_SYMBOLS ?
		       V34_PHASE3_S_SECTION :
		       V34_PHASE3_S_BAR_SECTION;
}

static bool test_normative_sequence(void)
{
	struct v34_phase3_s_sequence sequence = { UINT16_MAX };
	const double coordinate = sqrt(0.5);
	const struct qam_point point_zero = {
		coordinate,
		coordinate,
	};
	unsigned int section_counts[2] = { 0 };

	CHECK(V34_PHASE3_S_SYMBOLS == 128 &&
	      V34_PHASE3_S_BAR_SYMBOLS == 16 &&
	      V34_PHASE3_S_SEQUENCE_SYMBOLS == 144 &&
	      v34_phase3_s_sequence_init(&sequence) ==
		      V34_PHASE3_S_SEQUENCE_OK &&
	      v34_phase3_s_sequence_is_valid(&sequence) &&
	      !v34_phase3_s_sequence_is_complete(&sequence),
	      "S/S-bar sequence constants or initialization changed");
	for (unsigned int symbol = 0;
	     symbol < V34_PHASE3_S_SEQUENCE_SYMBOLS; ++symbol) {
		struct qam_point point = { 7.0, -9.0 };
		enum v34_phase3_s_section section =
			(enum v34_phase3_s_section)99;
		uint8_t quarter_turns = UINT8_MAX;
		double power;

		CHECK(v34_phase3_s_sequence_next(
			      &sequence, &point, &section) ==
			      V34_PHASE3_S_SEQUENCE_OK &&
		      qam_nearest_quarter_turn(
			      &point_zero, &point, &quarter_turns) ==
			      QAM_OK,
		      "S/S-bar symbol %u was not generated", symbol);
		power = point.in_phase * point.in_phase +
			point.quadrature * point.quadrature;
		CHECK(quarter_turns == expected_quarter_turns(symbol) &&
		      section == expected_section(symbol) &&
		      fabs(power - 1.0) < 1e-14 &&
		      sequence.emitted_symbols == symbol + 1u &&
		      v34_phase3_s_sequence_is_valid(&sequence),
		      "S/S-bar symbol %u has the wrong rotation or section",
		      symbol);
		section_counts[section]++;
	}
	CHECK(section_counts[V34_PHASE3_S_SECTION] ==
		      V34_PHASE3_S_SYMBOLS &&
	      section_counts[V34_PHASE3_S_BAR_SECTION] ==
		      V34_PHASE3_S_BAR_SYMBOLS &&
	      v34_phase3_s_sequence_is_complete(&sequence),
	      "S/S-bar section lengths changed");
	return true;
}

static bool test_completion_and_copy(void)
{
	struct v34_phase3_s_sequence first;
	struct v34_phase3_s_sequence second;
	struct qam_point ignored_point;
	enum v34_phase3_s_section ignored_section;

	CHECK(v34_phase3_s_sequence_init(&first) ==
		      V34_PHASE3_S_SEQUENCE_OK,
	      "first S/S-bar sequence did not initialize");
	for (unsigned int symbol = 0; symbol < 53; ++symbol)
		CHECK(v34_phase3_s_sequence_next(
			      &first, &ignored_point, &ignored_section) ==
			      V34_PHASE3_S_SEQUENCE_OK,
		      "S/S-bar prefix stopped at symbol %u", symbol);
	second = first;
	while (!v34_phase3_s_sequence_is_complete(&first)) {
		struct qam_point first_point;
		struct qam_point second_point;
		enum v34_phase3_s_section first_section;
		enum v34_phase3_s_section second_section;

		CHECK(v34_phase3_s_sequence_next(
			      &first, &first_point, &first_section) ==
			      V34_PHASE3_S_SEQUENCE_OK &&
		      v34_phase3_s_sequence_next(
			      &second, &second_point, &second_section) ==
			      V34_PHASE3_S_SEQUENCE_OK &&
		      first_point.in_phase == second_point.in_phase &&
		      first_point.quadrature == second_point.quadrature &&
		      first_section == second_section &&
		      first.emitted_symbols == second.emitted_symbols,
		      "copied S/S-bar sequence diverged at symbol %u",
		      first.emitted_symbols);
	}
	{
		struct qam_point point = { 7.0, -9.0 };
		struct qam_point before_point = point;
		enum v34_phase3_s_section section =
			(enum v34_phase3_s_section)99;
		struct v34_phase3_s_sequence before = first;

		CHECK(v34_phase3_s_sequence_next(
			      &first, &point, &section) ==
			      V34_PHASE3_S_SEQUENCE_COMPLETE &&
		      memcmp(&first, &before, sizeof(first)) == 0 &&
		      memcmp(&point, &before_point, sizeof(point)) == 0 &&
		      section == (enum v34_phase3_s_section)99,
		      "completed S/S-bar sequence mutated its outputs");
	}
	return true;
}

static bool test_validation(void)
{
	struct v34_phase3_s_sequence valid = { 0 };
	struct v34_phase3_s_sequence invalid = {
		.emitted_symbols = V34_PHASE3_S_SEQUENCE_SYMBOLS + 1,
	};
	struct v34_phase3_s_sequence before_valid = valid;
	struct v34_phase3_s_sequence before_invalid = invalid;
	struct qam_point point = { 7.0, -9.0 };
	struct qam_point before_point = point;
	enum v34_phase3_s_section section =
		(enum v34_phase3_s_section)99;

	CHECK(!v34_phase3_s_sequence_is_valid(NULL) &&
	      !v34_phase3_s_sequence_is_valid(&invalid) &&
	      !v34_phase3_s_sequence_is_complete(NULL) &&
	      !v34_phase3_s_sequence_is_complete(&invalid) &&
	      v34_phase3_s_sequence_init(NULL) ==
		      V34_PHASE3_S_SEQUENCE_INVALID_ARGUMENT,
	      "S/S-bar sequence accepted an invalid state");
	CHECK(v34_phase3_s_sequence_next(
		      &invalid, &point, &section) ==
		      V34_PHASE3_S_SEQUENCE_INVALID_ARGUMENT &&
	      memcmp(&invalid, &before_invalid, sizeof(invalid)) == 0 &&
	      memcmp(&point, &before_point, sizeof(point)) == 0 &&
	      section == (enum v34_phase3_s_section)99,
	      "invalid S/S-bar state mutated an output");
	CHECK(v34_phase3_s_sequence_next(
		      NULL, &point, &section) ==
		      V34_PHASE3_S_SEQUENCE_INVALID_ARGUMENT &&
	      v34_phase3_s_sequence_next(
		      &valid, NULL, &section) ==
		      V34_PHASE3_S_SEQUENCE_INVALID_ARGUMENT &&
	      v34_phase3_s_sequence_next(
		      &valid, &point, NULL) ==
		      V34_PHASE3_S_SEQUENCE_INVALID_ARGUMENT &&
	      memcmp(&valid, &before_valid, sizeof(valid)) == 0 &&
	      memcmp(&point, &before_point, sizeof(point)) == 0 &&
	      section == (enum v34_phase3_s_section)99,
	      "invalid S/S-bar argument mutated state");
	CHECK(strcmp(v34_phase3_s_sequence_result_name(
			     V34_PHASE3_S_SEQUENCE_COMPLETE),
		     "complete") == 0 &&
	      strcmp(v34_phase3_s_sequence_result_name(
			     (enum v34_phase3_s_sequence_result)99),
		     "unknown") == 0,
	      "S/S-bar result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "normative sequence", test_normative_sequence },
		{ "completion and copy", test_completion_and_copy },
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
