// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_v34_phase3_s_sequence.h>

#include "test.h"

#include <math.h>
#include <string.h>

static unsigned int expected_quarter_turns(unsigned int symbol)
{
	if (symbol < SIPPYMODEM_V34_PHASE3_S_SYMBOLS)
		return symbol & 1u;
	return 2u + ((symbol - SIPPYMODEM_V34_PHASE3_S_SYMBOLS) & 1u);
}

static enum sippymodem_v34_phase3_s_section expected_section(
	unsigned int symbol)
{
	return symbol < SIPPYMODEM_V34_PHASE3_S_SYMBOLS ?
		       SIPPYMODEM_V34_PHASE3_S_SECTION :
		       SIPPYMODEM_V34_PHASE3_S_BAR_SECTION;
}

static bool test_normative_sequence(void)
{
	struct sippymodem_v34_phase3_s_sequence sequence = { UINT16_MAX };
	const double coordinate = sqrt(0.5);
	const struct sippymodem_qam_point point_zero = {
		coordinate,
		coordinate,
	};
	unsigned int section_counts[2] = { 0 };

	CHECK(SIPPYMODEM_V34_PHASE3_S_SYMBOLS == 128 &&
	      SIPPYMODEM_V34_PHASE3_S_BAR_SYMBOLS == 16 &&
	      SIPPYMODEM_V34_PHASE3_S_SEQUENCE_SYMBOLS == 144 &&
	      sippymodem_v34_phase3_s_sequence_init(&sequence) ==
		      SIPPYMODEM_V34_PHASE3_S_SEQUENCE_OK &&
	      sippymodem_v34_phase3_s_sequence_is_valid(&sequence) &&
	      !sippymodem_v34_phase3_s_sequence_is_complete(&sequence),
	      "S/S-bar sequence constants or initialization changed");
	for (unsigned int symbol = 0;
	     symbol < SIPPYMODEM_V34_PHASE3_S_SEQUENCE_SYMBOLS; ++symbol) {
		struct sippymodem_qam_point point = { 7.0, -9.0 };
		enum sippymodem_v34_phase3_s_section section =
			(enum sippymodem_v34_phase3_s_section)99;
		uint8_t quarter_turns = UINT8_MAX;
		double power;

		CHECK(sippymodem_v34_phase3_s_sequence_next(
			      &sequence, &point, &section) ==
			      SIPPYMODEM_V34_PHASE3_S_SEQUENCE_OK &&
		      sippymodem_qam_nearest_quarter_turn(
			      &point_zero, &point, &quarter_turns) ==
			      SIPPYMODEM_QAM_OK,
		      "S/S-bar symbol %u was not generated", symbol);
		power = point.in_phase * point.in_phase +
			point.quadrature * point.quadrature;
		CHECK(quarter_turns == expected_quarter_turns(symbol) &&
		      section == expected_section(symbol) &&
		      fabs(power - 1.0) < 1e-14 &&
		      sequence.emitted_symbols == symbol + 1u &&
		      sippymodem_v34_phase3_s_sequence_is_valid(&sequence),
		      "S/S-bar symbol %u has the wrong rotation or section",
		      symbol);
		section_counts[section]++;
	}
	CHECK(section_counts[SIPPYMODEM_V34_PHASE3_S_SECTION] ==
		      SIPPYMODEM_V34_PHASE3_S_SYMBOLS &&
	      section_counts[SIPPYMODEM_V34_PHASE3_S_BAR_SECTION] ==
		      SIPPYMODEM_V34_PHASE3_S_BAR_SYMBOLS &&
	      sippymodem_v34_phase3_s_sequence_is_complete(&sequence),
	      "S/S-bar section lengths changed");
	return true;
}

static bool test_completion_and_copy(void)
{
	struct sippymodem_v34_phase3_s_sequence first;
	struct sippymodem_v34_phase3_s_sequence second;
	struct sippymodem_qam_point ignored_point;
	enum sippymodem_v34_phase3_s_section ignored_section;

	CHECK(sippymodem_v34_phase3_s_sequence_init(&first) ==
		      SIPPYMODEM_V34_PHASE3_S_SEQUENCE_OK,
	      "first S/S-bar sequence did not initialize");
	for (unsigned int symbol = 0; symbol < 53; ++symbol)
		CHECK(sippymodem_v34_phase3_s_sequence_next(
			      &first, &ignored_point, &ignored_section) ==
			      SIPPYMODEM_V34_PHASE3_S_SEQUENCE_OK,
		      "S/S-bar prefix stopped at symbol %u", symbol);
	second = first;
	while (!sippymodem_v34_phase3_s_sequence_is_complete(&first)) {
		struct sippymodem_qam_point first_point;
		struct sippymodem_qam_point second_point;
		enum sippymodem_v34_phase3_s_section first_section;
		enum sippymodem_v34_phase3_s_section second_section;

		CHECK(sippymodem_v34_phase3_s_sequence_next(
			      &first, &first_point, &first_section) ==
			      SIPPYMODEM_V34_PHASE3_S_SEQUENCE_OK &&
		      sippymodem_v34_phase3_s_sequence_next(
			      &second, &second_point, &second_section) ==
			      SIPPYMODEM_V34_PHASE3_S_SEQUENCE_OK &&
		      first_point.in_phase == second_point.in_phase &&
		      first_point.quadrature == second_point.quadrature &&
		      first_section == second_section &&
		      first.emitted_symbols == second.emitted_symbols,
		      "copied S/S-bar sequence diverged at symbol %u",
		      first.emitted_symbols);
	}
	{
		struct sippymodem_qam_point point = { 7.0, -9.0 };
		struct sippymodem_qam_point before_point = point;
		enum sippymodem_v34_phase3_s_section section =
			(enum sippymodem_v34_phase3_s_section)99;
		struct sippymodem_v34_phase3_s_sequence before = first;

		CHECK(sippymodem_v34_phase3_s_sequence_next(
			      &first, &point, &section) ==
			      SIPPYMODEM_V34_PHASE3_S_SEQUENCE_COMPLETE &&
		      memcmp(&first, &before, sizeof(first)) == 0 &&
		      memcmp(&point, &before_point, sizeof(point)) == 0 &&
		      section == (enum sippymodem_v34_phase3_s_section)99,
		      "completed S/S-bar sequence mutated its outputs");
	}
	return true;
}

static bool test_validation(void)
{
	struct sippymodem_v34_phase3_s_sequence valid = { 0 };
	struct sippymodem_v34_phase3_s_sequence invalid = {
		.emitted_symbols = SIPPYMODEM_V34_PHASE3_S_SEQUENCE_SYMBOLS + 1,
	};
	struct sippymodem_v34_phase3_s_sequence before_valid = valid;
	struct sippymodem_v34_phase3_s_sequence before_invalid = invalid;
	struct sippymodem_qam_point point = { 7.0, -9.0 };
	struct sippymodem_qam_point before_point = point;
	enum sippymodem_v34_phase3_s_section section =
		(enum sippymodem_v34_phase3_s_section)99;

	CHECK(!sippymodem_v34_phase3_s_sequence_is_valid(NULL) &&
	      !sippymodem_v34_phase3_s_sequence_is_valid(&invalid) &&
	      !sippymodem_v34_phase3_s_sequence_is_complete(NULL) &&
	      !sippymodem_v34_phase3_s_sequence_is_complete(&invalid) &&
	      sippymodem_v34_phase3_s_sequence_init(NULL) ==
		      SIPPYMODEM_V34_PHASE3_S_SEQUENCE_INVALID_ARGUMENT,
	      "S/S-bar sequence accepted an invalid state");
	CHECK(sippymodem_v34_phase3_s_sequence_next(
		      &invalid, &point, &section) ==
		      SIPPYMODEM_V34_PHASE3_S_SEQUENCE_INVALID_ARGUMENT &&
	      memcmp(&invalid, &before_invalid, sizeof(invalid)) == 0 &&
	      memcmp(&point, &before_point, sizeof(point)) == 0 &&
	      section == (enum sippymodem_v34_phase3_s_section)99,
	      "invalid S/S-bar state mutated an output");
	CHECK(sippymodem_v34_phase3_s_sequence_next(
		      NULL, &point, &section) ==
		      SIPPYMODEM_V34_PHASE3_S_SEQUENCE_INVALID_ARGUMENT &&
	      sippymodem_v34_phase3_s_sequence_next(
		      &valid, NULL, &section) ==
		      SIPPYMODEM_V34_PHASE3_S_SEQUENCE_INVALID_ARGUMENT &&
	      sippymodem_v34_phase3_s_sequence_next(
		      &valid, &point, NULL) ==
		      SIPPYMODEM_V34_PHASE3_S_SEQUENCE_INVALID_ARGUMENT &&
	      memcmp(&valid, &before_valid, sizeof(valid)) == 0 &&
	      memcmp(&point, &before_point, sizeof(point)) == 0 &&
	      section == (enum sippymodem_v34_phase3_s_section)99,
	      "invalid S/S-bar argument mutated state");
	CHECK(strcmp(sippymodem_v34_phase3_s_sequence_result_name(
			     SIPPYMODEM_V34_PHASE3_S_SEQUENCE_COMPLETE),
		     "complete") == 0 &&
	      strcmp(sippymodem_v34_phase3_s_sequence_result_name(
			     (enum sippymodem_v34_phase3_s_sequence_result)99),
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
