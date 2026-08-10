// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_v34_phase3_pp_sequence.h>

#include "test.h"

#include <math.h>
#include <string.h>

static const double TEST_PI =
	3.1415926535897932384626433832795;

static unsigned int expected_phase(unsigned int symbol)
{
	unsigned int period_symbol =
		symbol % SIPPYMODEM_V34_PHASE3_PP_PERIOD_SYMBOLS;
	unsigned int k = period_symbol / 4u;
	unsigned int i = period_symbol % 4u;
	unsigned int phase = k * i;

	if (k % 3u == 1u)
		phase += 4u;
	return phase % 12u;
}

static struct sippymodem_qam_point expected_point(unsigned int symbol)
{
	double angle = TEST_PI * expected_phase(symbol) / 6.0;

	return (struct sippymodem_qam_point){ cos(angle), sin(angle) };
}

static bool test_normative_sequence(void)
{
	struct sippymodem_v34_phase3_pp_sequence sequence = { UINT16_MAX };
	struct sippymodem_qam_point first_period[
		SIPPYMODEM_V34_PHASE3_PP_PERIOD_SYMBOLS];
	unsigned int phase_mask = 0;

	CHECK(SIPPYMODEM_V34_PHASE3_PP_PERIOD_SYMBOLS == 48 &&
	      SIPPYMODEM_V34_PHASE3_PP_PERIODS == 6 &&
	      SIPPYMODEM_V34_PHASE3_PP_SEQUENCE_SYMBOLS == 288 &&
	      sippymodem_v34_phase3_pp_sequence_init(&sequence) ==
		      SIPPYMODEM_V34_PHASE3_PP_SEQUENCE_OK &&
	      sippymodem_v34_phase3_pp_sequence_is_valid(&sequence) &&
	      !sippymodem_v34_phase3_pp_sequence_is_complete(&sequence),
	      "PP sequence constants or initialization changed");
	for (unsigned int symbol = 0;
	     symbol < SIPPYMODEM_V34_PHASE3_PP_SEQUENCE_SYMBOLS; ++symbol) {
		struct sippymodem_qam_point point = { 7.0, -9.0 };
		struct sippymodem_qam_point expected = expected_point(symbol);
		double power;

		CHECK(sippymodem_v34_phase3_pp_sequence_next(
			      &sequence, &point) ==
			      SIPPYMODEM_V34_PHASE3_PP_SEQUENCE_OK,
		      "PP symbol %u was not generated", symbol);
		power = point.in_phase * point.in_phase +
			point.quadrature * point.quadrature;
		CHECK(fabs(point.in_phase - expected.in_phase) < 4e-15 &&
		      fabs(point.quadrature - expected.quadrature) < 4e-15 &&
		      fabs(power - 1.0) < 4e-15 &&
		      sequence.emitted_symbols == symbol + 1u &&
		      sippymodem_v34_phase3_pp_sequence_is_valid(&sequence),
		      "PP symbol %u differs from equation 10-1", symbol);
		phase_mask |= 1u << expected_phase(symbol);
		if (symbol < SIPPYMODEM_V34_PHASE3_PP_PERIOD_SYMBOLS) {
			first_period[symbol] = point;
		} else {
			unsigned int period_symbol =
				symbol %
				SIPPYMODEM_V34_PHASE3_PP_PERIOD_SYMBOLS;

			CHECK(point.in_phase ==
				      first_period[period_symbol].in_phase &&
			      point.quadrature ==
				      first_period[period_symbol].quadrature,
			      "PP period changed at symbol %u", symbol);
		}
	}
	CHECK(phase_mask == (1u << 12) - 1u &&
	      sippymodem_v34_phase3_pp_sequence_is_complete(&sequence),
	      "PP sequence did not cover every twelfth root or complete");
	return true;
}

static bool test_copy_and_completion(void)
{
	struct sippymodem_v34_phase3_pp_sequence first;
	struct sippymodem_v34_phase3_pp_sequence second;
	struct sippymodem_qam_point ignored;

	CHECK(sippymodem_v34_phase3_pp_sequence_init(&first) ==
		      SIPPYMODEM_V34_PHASE3_PP_SEQUENCE_OK,
	      "PP copy fixture did not initialize");
	for (unsigned int symbol = 0; symbol < 101; ++symbol)
		CHECK(sippymodem_v34_phase3_pp_sequence_next(
			      &first, &ignored) ==
			      SIPPYMODEM_V34_PHASE3_PP_SEQUENCE_OK,
		      "PP prefix stopped at symbol %u", symbol);
	second = first;
	while (!sippymodem_v34_phase3_pp_sequence_is_complete(&first)) {
		struct sippymodem_qam_point first_point;
		struct sippymodem_qam_point second_point;

		CHECK(sippymodem_v34_phase3_pp_sequence_next(
			      &first, &first_point) ==
			      SIPPYMODEM_V34_PHASE3_PP_SEQUENCE_OK &&
		      sippymodem_v34_phase3_pp_sequence_next(
			      &second, &second_point) ==
			      SIPPYMODEM_V34_PHASE3_PP_SEQUENCE_OK &&
		      memcmp(&first_point, &second_point,
			     sizeof(first_point)) == 0 &&
		      memcmp(&first, &second, sizeof(first)) == 0,
		      "copied PP sequence diverged at symbol %u",
		      first.emitted_symbols);
	}
	{
		struct sippymodem_v34_phase3_pp_sequence before = first;
		struct sippymodem_qam_point point = { 7.0, -9.0 };
		struct sippymodem_qam_point before_point = point;

		CHECK(sippymodem_v34_phase3_pp_sequence_next(
			      &first, &point) ==
			      SIPPYMODEM_V34_PHASE3_PP_SEQUENCE_COMPLETE &&
		      memcmp(&first, &before, sizeof(first)) == 0 &&
		      memcmp(&point, &before_point, sizeof(point)) == 0,
		      "completed PP sequence mutated state or output");
	}
	return true;
}

static bool test_validation(void)
{
	struct sippymodem_v34_phase3_pp_sequence valid = { 0 };
	struct sippymodem_v34_phase3_pp_sequence invalid = {
		.emitted_symbols =
			SIPPYMODEM_V34_PHASE3_PP_SEQUENCE_SYMBOLS + 1,
	};
	struct sippymodem_v34_phase3_pp_sequence before_valid = valid;
	struct sippymodem_v34_phase3_pp_sequence before_invalid = invalid;
	struct sippymodem_qam_point point = { 7.0, -9.0 };
	struct sippymodem_qam_point before_point = point;

	CHECK(!sippymodem_v34_phase3_pp_sequence_is_valid(NULL) &&
	      !sippymodem_v34_phase3_pp_sequence_is_valid(&invalid) &&
	      !sippymodem_v34_phase3_pp_sequence_is_complete(NULL) &&
	      !sippymodem_v34_phase3_pp_sequence_is_complete(&invalid) &&
	      sippymodem_v34_phase3_pp_sequence_init(NULL) ==
		      SIPPYMODEM_V34_PHASE3_PP_SEQUENCE_INVALID_ARGUMENT,
	      "PP sequence accepted invalid state");
	CHECK(sippymodem_v34_phase3_pp_sequence_next(
		      &invalid, &point) ==
		      SIPPYMODEM_V34_PHASE3_PP_SEQUENCE_INVALID_ARGUMENT &&
	      memcmp(&invalid, &before_invalid, sizeof(invalid)) == 0 &&
	      memcmp(&point, &before_point, sizeof(point)) == 0,
	      "invalid PP state mutated an output");
	CHECK(sippymodem_v34_phase3_pp_sequence_next(
		      NULL, &point) ==
		      SIPPYMODEM_V34_PHASE3_PP_SEQUENCE_INVALID_ARGUMENT &&
	      sippymodem_v34_phase3_pp_sequence_next(
		      &valid, NULL) ==
		      SIPPYMODEM_V34_PHASE3_PP_SEQUENCE_INVALID_ARGUMENT &&
	      memcmp(&valid, &before_valid, sizeof(valid)) == 0 &&
	      memcmp(&point, &before_point, sizeof(point)) == 0,
	      "invalid PP argument mutated state");
	CHECK(strcmp(sippymodem_v34_phase3_pp_sequence_result_name(
			     SIPPYMODEM_V34_PHASE3_PP_SEQUENCE_COMPLETE),
		     "complete") == 0 &&
	      strcmp(sippymodem_v34_phase3_pp_sequence_result_name(
			     (enum sippymodem_v34_phase3_pp_sequence_result)99),
		     "unknown") == 0,
	      "PP result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "normative sequence", test_normative_sequence },
		{ "copy and completion", test_copy_and_completion },
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
