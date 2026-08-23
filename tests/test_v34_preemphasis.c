// SPDX-License-Identifier: BSD-3-Clause

#include <v34_preemphasis.h>

#include "test.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

enum {
	TEST_SAMPLE_RATE = 8000,
	TEST_PROFILE_COUNT = 10,
	TEST_Q14_SCALE = 16384,
};

struct profile_case {
	uint32_t symbol_rate_numerator;
	uint32_t symbol_rate_denominator;
	uint64_t expected_hash;
};

static const struct profile_case PROFILE_CASES[] = {
	{ 2400, 1, UINT64_C(0xf9da0b31fad14155) },
	{ 19200, 7, UINT64_C(0xeef5da9dbd001da9) },
	{ 2800, 1, UINT64_C(0xeef5da9dbd001da9) },
	{ 3000, 1, UINT64_C(0x3b7abe7359a3ed19) },
	{ 3200, 1, UINT64_C(0xfa96776d64bc6cc5) },
	{ 24000, 7, UINT64_C(0xeba1df48f561ba0d) },
};

static uint64_t hash_word(uint64_t hash, uint16_t word)
{
	hash ^= (uint8_t)word;
	hash *= UINT64_C(1099511628211);
	hash ^= (uint8_t)(word >> 8);
	return hash * UINT64_C(1099511628211);
}

static bool test_fixed_profiles(void)
{
	struct v34_preemphasis identity;

	CHECK(V34_PREEMPHASIS_TAPS == 16,
	      "pre-emphasis FIR size changed");
	CHECK(v34_preemphasis_init(
		      &identity, TEST_SAMPLE_RATE, 2400, 1, 0) ==
		      V34_PREEMPHASIS_OK,
	      "identity pre-emphasis did not initialize");
	for (size_t tap = 0; tap < V34_PREEMPHASIS_TAPS; ++tap)
		CHECK(identity.coefficients[tap] == (tap == 7 ? 1.0 : 0.0),
		      "identity pre-emphasis changed at tap %zu", tap);

	for (size_t profile_case = 0;
	     profile_case < ARRAY_LENGTH(PROFILE_CASES); ++profile_case) {
		const struct profile_case *profile =
			&PROFILE_CASES[profile_case];
		uint64_t hash = UINT64_C(14695981039346656037);

		for (uint8_t filter_index = 1;
		     filter_index <= TEST_PROFILE_COUNT;
		     ++filter_index) {
			struct v34_preemphasis filter;

			CHECK(v34_preemphasis_init(
				      &filter, TEST_SAMPLE_RATE,
				      profile->symbol_rate_numerator,
				      profile->symbol_rate_denominator,
				      filter_index) ==
				      V34_PREEMPHASIS_OK &&
			      v34_preemphasis_is_valid(&filter) &&
			      filter.filter_index == filter_index,
			      "pre-emphasis filter %u did not initialize for rate %zu",
			      filter_index, profile_case);
			for (size_t tap = 0; tap < V34_PREEMPHASIS_TAPS;
			     ++tap) {
				long q14 = lround(
					filter.coefficients[tap] * TEST_Q14_SCALE);

				CHECK(q14 >= INT16_MIN && q14 <= INT16_MAX &&
				      filter.coefficients[tap] ==
					      (double)q14 / TEST_Q14_SCALE,
				      "pre-emphasis profile %zu:%u tap %zu was not exact Q14",
				      profile_case, filter_index, tap);
				hash = hash_word(hash, (uint16_t)(int16_t)q14);
			}
		}
		CHECK(hash == profile->expected_hash,
		      "pre-emphasis profile %zu changed", profile_case);
	}
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
	      memcmp(&invalid, &before, sizeof(invalid)) == 0,
	      "invalid pre-emphasis state was accepted");
	invalid = filter;
	invalid.coefficients[0] += 0.25;
	CHECK(!v34_preemphasis_is_valid(&invalid),
	      "asymmetric pre-emphasis coefficients were accepted");
	invalid = filter;
	invalid.history[0] = NAN;
	CHECK(!v34_preemphasis_is_valid(&invalid),
	      "non-finite pre-emphasis history was accepted");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "fixed profiles", test_fixed_profiles },
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
