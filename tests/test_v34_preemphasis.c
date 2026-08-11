// SPDX-License-Identifier: BSD-3-Clause

#include <v34_preemphasis.h>

#include "test.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

enum {
	TEST_SAMPLE_RATE = 8000,
	TEST_BAND_STEPS = 90,
};

static const double TEST_TEMPLATE_LIMIT = 1.2;
static const double TEST_BAND_HALF_WIDTH = 0.45;
static const double TEST_RESPONSE_ERROR_DB = 0.55;

struct v34_band {
	uint32_t symbol_rate_numerator;
	uint32_t symbol_rate_denominator;
	uint32_t carrier_ratio_numerator;
	uint32_t carrier_ratio_denominator;
};

static const struct v34_band V34_BANDS[] = {
	{ 2400, 1, 2, 3 },
	{ 2400, 1, 3, 4 },
	{ 19200, 7, 3, 5 },
	{ 19200, 7, 2, 3 },
	{ 2800, 1, 3, 5 },
	{ 2800, 1, 2, 3 },
	{ 3000, 1, 3, 5 },
	{ 3000, 1, 2, 3 },
	{ 3200, 1, 4, 7 },
	{ 3200, 1, 3, 5 },
	{ 24000, 7, 4, 7 },
};

static bool check_template_point(
	uint8_t filter_index, double frequency, double expected_db,
	bool expected_constrained)
{
	double magnitude_db = -99.0;
	bool constrained = !expected_constrained;

	CHECK(v34_preemphasis_template_db(
		      filter_index, frequency, &magnitude_db, &constrained) ==
		      V34_PREEMPHASIS_OK &&
	      fabs(magnitude_db - expected_db) < 1e-12 &&
	      constrained == expected_constrained,
	      "pre-emphasis index %u template changed at %.1f S",
	      filter_index, frequency);
	return true;
}

static bool test_normative_templates(void)
{
	for (uint8_t filter_index = 0; filter_index <= 5; ++filter_index) {
		CHECK(check_template_point(filter_index, 0.0, 0.0, true) &&
		      check_template_point(
			      filter_index, 1.0, 2.0 * filter_index, true) &&
		      check_template_point(
			      filter_index, TEST_TEMPLATE_LIMIT,
			      2.0 * filter_index * TEST_TEMPLATE_LIMIT, true),
		      "sloping pre-emphasis template %u failed", filter_index);
	}
	for (uint8_t filter_index = 6;
	     filter_index <= V34_PREEMPHASIS_MAX_FILTER_INDEX;
	     ++filter_index) {
		double beta = 0.5 * (filter_index - 5u);

		CHECK(check_template_point(filter_index, 0.0, 0.0, true) &&
		      check_template_point(filter_index, 0.4, 0.0, true) &&
		      check_template_point(
			      filter_index, 0.6, beta * 0.5, false) &&
		      check_template_point(filter_index, 0.8, beta, true) &&
		      check_template_point(
			      filter_index, TEST_TEMPLATE_LIMIT,
			      3.0 * beta, true),
		      "split pre-emphasis template %u failed", filter_index);
	}
	return true;
}

static bool test_generated_responses(void)
{
	unsigned int constrained_points = 0;
	unsigned int bridged_points = 0;
	double worst_error = 0.0;

	CHECK(V34_PREEMPHASIS_TAPS == 21 &&
	      V34_PREEMPHASIS_DELAY_SAMPLES == 10,
	      "pre-emphasis FIR size or delay changed");
	for (size_t band_index = 0; band_index < ARRAY_LENGTH(V34_BANDS);
	     ++band_index) {
		const struct v34_band *band = &V34_BANDS[band_index];
		double carrier = (double)band->carrier_ratio_numerator /
				 band->carrier_ratio_denominator;

		for (uint8_t filter_index = 0;
		     filter_index <=
			     V34_PREEMPHASIS_MAX_FILTER_INDEX;
		     ++filter_index) {
			struct v34_preemphasis filter;
			double coefficient_sum = 0.0;

			CHECK(v34_preemphasis_init(
				      &filter, TEST_SAMPLE_RATE,
				      band->symbol_rate_numerator,
				      band->symbol_rate_denominator,
				      filter_index) ==
				      V34_PREEMPHASIS_OK &&
			      v34_preemphasis_is_valid(&filter) &&
			      filter.filter_index == filter_index,
			      "pre-emphasis filter %u did not initialize for band %zu",
			      filter_index, band_index);
			for (size_t tap = 0;
			     tap < V34_PREEMPHASIS_TAPS; ++tap) {
				CHECK(filter.coefficients[tap] ==
					      filter.coefficients[
						      V34_PREEMPHASIS_TAPS -
						      1u - tap],
				      "pre-emphasis filter %u lost linear phase",
				      filter_index);
				coefficient_sum += filter.coefficients[tap];
			}
			CHECK(fabs(coefficient_sum - 1.0) < 1e-12 &&
			      v34_preemphasis_peak_gain_bound(
				      &filter) >= 1.0 &&
			      v34_preemphasis_peak_gain_bound(
				      &filter) < 5.0,
			      "pre-emphasis filter %u normalization changed",
			      filter_index);
			for (unsigned int step = 0; step <= TEST_BAND_STEPS;
			     ++step) {
				double normalized_frequency =
					carrier - TEST_BAND_HALF_WIDTH +
					2.0 * TEST_BAND_HALF_WIDTH * step /
					TEST_BAND_STEPS;
				double target_db;
				double response_db;
				double error;
				bool constrained;

				CHECK(v34_preemphasis_template_db(
					      filter_index, normalized_frequency,
					      &target_db, &constrained) ==
					      V34_PREEMPHASIS_OK &&
				      v34_preemphasis_response_db(
					      &filter, normalized_frequency,
					      &response_db) ==
					      V34_PREEMPHASIS_OK,
				      "pre-emphasis response %u:%zu:%u failed",
				      filter_index, band_index, step);
				error = fabs(response_db - target_db);
				if (error > worst_error)
					worst_error = error;
				CHECK(error <= TEST_RESPONSE_ERROR_DB,
				      "pre-emphasis response %u:%zu is %.3f dB off at %.3f S",
				      filter_index, band_index, error,
				      normalized_frequency);
				if (constrained)
					constrained_points++;
				else
					bridged_points++;
			}
		}
	}
	CHECK(constrained_points > bridged_points && bridged_points > 0 &&
	      worst_error < TEST_RESPONSE_ERROR_DB,
	      "pre-emphasis response coverage was incomplete (%.3f dB)",
	      worst_error);
	return true;
}

static uint32_t next_random(uint32_t *state)
{
	*state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
	return *state;
}

static bool test_streaming_filter(void)
{
	struct v34_preemphasis first;
	struct v34_preemphasis second;
	uint32_t random = UINT32_C(0x3d4a29c7);

	CHECK(v34_preemphasis_init(
		      &first, TEST_SAMPLE_RATE, 2400, 1, 5) ==
		      V34_PREEMPHASIS_OK,
	      "pre-emphasis impulse fixture did not initialize");
	for (size_t sample = 0;
	     sample < V34_PREEMPHASIS_TAPS + 5u; ++sample) {
		double output = -99.0;
		double expected = sample < V34_PREEMPHASIS_TAPS ?
				  first.coefficients[sample] :
				  0.0;

		CHECK(v34_preemphasis_sample(
			      &first, sample == 0 ? 1.0 : 0.0, &output) ==
			      V34_PREEMPHASIS_OK &&
		      fabs(output - expected) < 1e-14,
		      "pre-emphasis impulse response changed at sample %zu",
		      sample);
	}
	for (unsigned int sample = 0; sample < 37; ++sample) {
		double input = (double)(int32_t)next_random(&random) / INT32_MAX;
		double output;

		CHECK(v34_preemphasis_sample(
			      &first, input, &output) ==
			      V34_PREEMPHASIS_OK,
		      "pre-emphasis prefix failed at sample %u", sample);
	}
	second = first;
	for (unsigned int sample = 0; sample < 64; ++sample) {
		double input = (double)(int32_t)next_random(&random) / INT32_MAX;
		double first_output;
		double second_output;

		CHECK(v34_preemphasis_sample(
			      &first, input, &first_output) ==
			      V34_PREEMPHASIS_OK &&
		      v34_preemphasis_sample(
			      &second, input, &second_output) ==
			      V34_PREEMPHASIS_OK &&
		      first_output == second_output &&
		      memcmp(&first, &second, sizeof(first)) == 0,
		      "copied pre-emphasis filter diverged at sample %u", sample);
	}
	CHECK(v34_preemphasis_reset(&first) ==
		      V34_PREEMPHASIS_OK &&
	      first.next_history == 0 &&
	      memcmp(first.coefficients, second.coefficients,
		     sizeof(first.coefficients)) == 0,
	      "pre-emphasis reset changed its configuration");
	for (size_t sample = 0; sample < ARRAY_LENGTH(first.history); ++sample)
		CHECK(first.history[sample] == 0.0,
		      "pre-emphasis reset retained sample %zu", sample);
	CHECK(v34_preemphasis_init(
		      &first, TEST_SAMPLE_RATE, 5600, 2, 3) ==
		      V34_PREEMPHASIS_OK &&
	      first.symbol_rate_numerator == 2800 &&
	      first.symbol_rate_denominator == 1,
	      "pre-emphasis rate fraction was not reduced");
	return true;
}

static bool test_validation(void)
{
	struct v34_preemphasis filter;
	struct v34_preemphasis before;
	struct v34_preemphasis target;
	struct v34_preemphasis before_target;
	struct v34_preemphasis invalid;
	struct v34_preemphasis huge;
	double output = -99.0;
	double before_output = output;
	bool constrained = true;
	bool before_constrained = constrained;

	memset(&target, 0xa5, sizeof(target));
	before_target = target;
	CHECK(v34_preemphasis_init(
		      NULL, TEST_SAMPLE_RATE, 2400, 1, 0) ==
		      V34_PREEMPHASIS_INVALID_ARGUMENT &&
	      v34_preemphasis_init(
		      &target, 0, 2400, 1, 0) ==
		      V34_PREEMPHASIS_INVALID_ARGUMENT &&
	      v34_preemphasis_init(
		      &target, TEST_SAMPLE_RATE, 0, 1, 0) ==
		      V34_PREEMPHASIS_INVALID_ARGUMENT &&
	      v34_preemphasis_init(
		      &target, TEST_SAMPLE_RATE, 2400, 0, 0) ==
		      V34_PREEMPHASIS_INVALID_ARGUMENT &&
	      v34_preemphasis_init(
		      &target, 4800, 2400, 1, 0) ==
		      V34_PREEMPHASIS_INVALID_ARGUMENT &&
	      v34_preemphasis_init(
		      &target, TEST_SAMPLE_RATE, 2400, 1, 11) ==
		      V34_PREEMPHASIS_INVALID_ARGUMENT &&
	      memcmp(&target, &before_target, sizeof(target)) == 0,
	      "invalid pre-emphasis initialization mutated its target");
	CHECK(v34_preemphasis_init(
		      &filter, TEST_SAMPLE_RATE, 2400, 1, 5) ==
		      V34_PREEMPHASIS_OK,
	      "pre-emphasis validation fixture did not initialize");
	before = filter;
	CHECK(v34_preemphasis_sample(NULL, 0.0, &output) ==
		      V34_PREEMPHASIS_INVALID_ARGUMENT &&
	      v34_preemphasis_sample(&filter, NAN, &output) ==
		      V34_PREEMPHASIS_INVALID_ARGUMENT &&
	      v34_preemphasis_sample(&filter, INFINITY, &output) ==
		      V34_PREEMPHASIS_INVALID_ARGUMENT &&
	      v34_preemphasis_sample(&filter, 0.0, NULL) ==
		      V34_PREEMPHASIS_INVALID_ARGUMENT &&
	      memcmp(&filter, &before, sizeof(filter)) == 0 &&
	      output == before_output,
	      "invalid pre-emphasis sample mutated state");
	huge = filter;
	for (size_t tap = 0; tap < V34_PREEMPHASIS_TAPS; ++tap) {
		size_t previous =
			(huge.next_history + V34_PREEMPHASIS_TAPS - tap) %
			V34_PREEMPHASIS_TAPS;

		huge.history[previous] =
			copysign(DBL_MAX, huge.coefficients[tap]);
	}
	before = huge;
	CHECK(v34_preemphasis_sample(
		      &huge, copysign(DBL_MAX, huge.coefficients[0]), &output) ==
		      V34_PREEMPHASIS_NUMERIC_FAILURE &&
	      memcmp(&huge, &before, sizeof(huge)) == 0 &&
	      output == before_output,
	      "overflowing pre-emphasis sample was not transactional");
	invalid = filter;
	invalid.next_history = V34_PREEMPHASIS_TAPS;
	before = invalid;
	CHECK(!v34_preemphasis_is_valid(NULL) &&
	      !v34_preemphasis_is_valid(&invalid) &&
	      v34_preemphasis_reset(NULL) ==
		      V34_PREEMPHASIS_INVALID_ARGUMENT &&
	      v34_preemphasis_reset(&invalid) ==
		      V34_PREEMPHASIS_INVALID_ARGUMENT &&
	      memcmp(&invalid, &before, sizeof(invalid)) == 0 &&
	      v34_preemphasis_peak_gain_bound(NULL) == 0.0 &&
	      v34_preemphasis_peak_gain_bound(&invalid) == 0.0,
	      "invalid pre-emphasis state was accepted");
	CHECK(v34_preemphasis_response_db(
		      NULL, 0.0, &output) ==
		      V34_PREEMPHASIS_INVALID_ARGUMENT &&
	      v34_preemphasis_response_db(
		      &filter, -0.1, &output) ==
		      V34_PREEMPHASIS_INVALID_ARGUMENT &&
	      v34_preemphasis_response_db(
		      &filter, NAN, &output) ==
		      V34_PREEMPHASIS_INVALID_ARGUMENT &&
	      v34_preemphasis_response_db(
		      &filter, 2.0, &output) ==
		      V34_PREEMPHASIS_INVALID_ARGUMENT &&
	      v34_preemphasis_response_db(
		      &filter, 0.0, NULL) ==
		      V34_PREEMPHASIS_INVALID_ARGUMENT &&
	      output == before_output,
	      "invalid pre-emphasis response query mutated its output");
	CHECK(v34_preemphasis_template_db(
		      11, 0.0, &output, &constrained) ==
		      V34_PREEMPHASIS_INVALID_ARGUMENT &&
	      v34_preemphasis_template_db(
		      0, -0.1, &output, &constrained) ==
		      V34_PREEMPHASIS_INVALID_ARGUMENT &&
	      v34_preemphasis_template_db(
		      0, 1.21, &output, &constrained) ==
		      V34_PREEMPHASIS_INVALID_ARGUMENT &&
	      v34_preemphasis_template_db(
		      0, 0.0, NULL, &constrained) ==
		      V34_PREEMPHASIS_INVALID_ARGUMENT &&
	      v34_preemphasis_template_db(
		      0, 0.0, &output, NULL) ==
		      V34_PREEMPHASIS_INVALID_ARGUMENT &&
	      output == before_output && constrained == before_constrained,
	      "invalid pre-emphasis template query mutated its outputs");
	invalid = filter;
	invalid.coefficients[0] += 0.25;
	CHECK(!v34_preemphasis_is_valid(&invalid),
	      "asymmetric pre-emphasis coefficients were accepted");
	invalid = filter;
	invalid.history[0] = NAN;
	CHECK(!v34_preemphasis_is_valid(&invalid),
	      "non-finite pre-emphasis history was accepted");
	CHECK(strcmp(v34_preemphasis_result_name(
			     V34_PREEMPHASIS_OK),
		     "ok") == 0 &&
	      strcmp(v34_preemphasis_result_name(
			     V34_PREEMPHASIS_NUMERIC_FAILURE),
		     "numeric failure") == 0 &&
	      strcmp(v34_preemphasis_result_name(
			     (enum v34_preemphasis_result)99),
		     "unknown") == 0,
	      "pre-emphasis result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "normative templates", test_normative_templates },
		{ "generated responses", test_generated_responses },
		{ "streaming filter", test_streaming_filter },
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
