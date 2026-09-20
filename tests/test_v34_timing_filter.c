// SPDX-License-Identifier: BSD-3-Clause

#include <v34_timing_filter.h>

#include "test.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static uint64_t hash_word(uint64_t hash, uint16_t word)
{
	for (unsigned int byte = 0; byte < 2; ++byte) {
		hash ^= (word >> (byte * 8u)) & 0xffu;
		hash *= UINT64_C(1099511628211);
	}
	return hash;
}

static uint64_t hash_u32(uint64_t hash, uint32_t value)
{
	hash = hash_word(hash, (uint16_t)value);
	return hash_word(hash, (uint16_t)(value >> 16));
}

static uint64_t hash_state(const struct v34_timing_filter *filter)
{
	uint64_t hash = UINT64_C(1469598103934665603);

	for (size_t index = 0;
	     index < V34_TIMING_FILTER_COMPLEX_TAPS; ++index) {
		hash = hash_word(
			hash, (uint16_t)filter->positive_b_real[index]);
		hash = hash_word(
			hash, (uint16_t)filter->positive_b_imag[index]);
		hash = hash_word(
			hash, (uint16_t)filter->positive_a_real[index]);
		hash = hash_word(
			hash, (uint16_t)filter->positive_a_imag[index]);
		hash = hash_word(
			hash, (uint16_t)filter->negative_a_real[index]);
		hash = hash_word(
			hash, (uint16_t)filter->negative_a_imag[index]);
	}
	for (size_t index = 0; index < V34_TIMING_FILTER_FIR_TAPS; ++index) {
		hash = hash_word(hash, (uint16_t)filter->error_history[index]);
		hash = hash_u32(hash, filter->prefilter_history[index]);
	}
	hash = hash_word(hash, (uint16_t)filter->current.in_phase);
	hash = hash_word(hash, (uint16_t)filter->current.quadrature);
	hash = hash_word(hash, (uint16_t)filter->previous.in_phase);
	return hash_word(hash, (uint16_t)filter->previous.quadrature);
}

static uint32_t next_word(uint32_t *state)
{
	*state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
	return *state;
}

static bool test_exact_filter(void)
{
	struct v34_timing_filter filter;
	uint64_t error_hash = UINT64_C(1469598103934665603);
	uint64_t sample_hash = UINT64_C(1469598103934665603);
	uint32_t random = UINT32_C(0x5a17c3e9);
	unsigned int sample_count = 0;

	CHECK(v34_timing_filter_init(&filter) == V34_TIMING_FILTER_OK,
	      "V.34 timing filter did not initialize");
	for (unsigned int group = 0; group < 2048; ++group) {
		for (unsigned int slot = 0; slot < 7; ++slot) {
			uint32_t word = next_word(&random);
			struct v34_analytic_sample input = {
				.in_phase = (int16_t)(uint16_t)word,
				.quadrature = (int16_t)(uint16_t)(word >> 16),
			};
			int16_t error;

			CHECK(v34_timing_filter_observe(
				      &filter, input, &error) ==
				      V34_TIMING_FILTER_OK,
			      "V.34 timing filter failed at %u/%u", group,
			      slot);
			error_hash = hash_word(error_hash, (uint16_t)error);
			if (slot == 1 || slot == 3 || slot == 5) {
				struct v34_analytic_sample output;
				uint32_t previous_history[V34_TIMING_FILTER_FIR_TAPS];

				memcpy(previous_history, filter.prefilter_history,
				       sizeof(previous_history));
				CHECK(v34_timing_filter_prefilter(
					      &filter, &output) ==
					      V34_TIMING_FILTER_OK,
				      "V.34 timing prefilter failed at %u/%u",
				      group, slot);
				CHECK(filter.prefilter_history[0] ==
					      (((uint32_t)(uint16_t)filter.current.quadrature << 16) |
					       (uint16_t)filter.current.in_phase) &&
				      filter.prefilter_history[1] ==
					      (((uint32_t)(uint16_t)filter.previous.quadrature << 16) |
					       (uint16_t)filter.previous.in_phase) &&
				      memcmp(filter.prefilter_history + 2, previous_history,
					     sizeof(previous_history) -
					     2 * sizeof(previous_history[0])) == 0,
				      "V.34 prefilter pair history changed at %u/%u",
				      group, slot);
				sample_hash = hash_word(
					sample_hash, (uint16_t)output.in_phase);
				sample_hash = hash_word(
					sample_hash, (uint16_t)output.quadrature);
				++sample_count;
			}
		}
	}
	CHECK(random == UINT32_C(0x83a9bbe9) && sample_count == 6144 &&
		      error_hash == UINT64_C(0x3fa6c7cad9493048) &&
		      sample_hash == UINT64_C(0x6b2dda474bdb0961) &&
		      hash_state(&filter) == UINT64_C(0xd05e5590a587652a),
	      "V.34 timing oracle changed: %#x/%u/%#llx/%#llx/%#llx",
	      random, sample_count, (unsigned long long)error_hash,
	      (unsigned long long)sample_hash,
	      (unsigned long long)hash_state(&filter));
	return true;
}

static bool test_ownership(void)
{
	struct v34_timing_filter left;
	struct v34_timing_filter right;
	struct v34_timing_filter saved;
	struct v34_timing_filter uninitialized = { 0 };
	struct v34_analytic_sample output = { 123, -456 };
	struct v34_analytic_sample saved_output = output;
	int16_t error = 789;
	int16_t saved_error = error;
	uint32_t random = UINT32_C(0xa341316c);

	CHECK(v34_timing_filter_init(&left) == V34_TIMING_FILTER_OK &&
		      v34_timing_filter_init(&right) == V34_TIMING_FILTER_OK &&
		      !v34_timing_filter_is_valid(NULL) &&
		      !v34_timing_filter_is_valid(&uninitialized) &&
		      v34_timing_filter_init(NULL) ==
			      V34_TIMING_FILTER_INVALID_ARGUMENT,
	      "V.34 timing lifecycle changed");
	saved = left;
	CHECK(v34_timing_filter_observe(
		      NULL, (struct v34_analytic_sample){ 0, 0 }, &error) ==
		      V34_TIMING_FILTER_INVALID_STATE &&
		      v34_timing_filter_observe(
			      &uninitialized,
			      (struct v34_analytic_sample){ 0, 0 }, &error) ==
			      V34_TIMING_FILTER_INVALID_STATE &&
		      v34_timing_filter_observe(
			      &left, (struct v34_analytic_sample){ 0, 0 }, NULL) ==
			      V34_TIMING_FILTER_INVALID_ARGUMENT &&
		      v34_timing_filter_observe(
			      &left, (struct v34_analytic_sample){ 0, 0 },
			      (int16_t *)&left) ==
			      V34_TIMING_FILTER_INVALID_ARGUMENT &&
		      v34_timing_filter_prefilter(NULL, &output) ==
			      V34_TIMING_FILTER_INVALID_STATE &&
		      v34_timing_filter_prefilter(&uninitialized, &output) ==
			      V34_TIMING_FILTER_INVALID_STATE &&
		      v34_timing_filter_prefilter(&left, NULL) ==
			      V34_TIMING_FILTER_INVALID_ARGUMENT &&
		      v34_timing_filter_prefilter(
			      &left, (struct v34_analytic_sample *)&left) ==
			      V34_TIMING_FILTER_INVALID_ARGUMENT &&
		      memcmp(&left, &saved, sizeof(left)) == 0 &&
		      error == saved_error &&
		      memcmp(&output, &saved_output, sizeof(output)) == 0,
	      "V.34 timing rejection was not transactional");

	for (unsigned int index = 0; index < 512; ++index) {
		uint32_t word = next_word(&random);
		struct v34_analytic_sample input = {
			.in_phase = (int16_t)(uint16_t)word,
			.quadrature = (int16_t)(uint16_t)(word >> 16),
		};
		int16_t left_error;
		int16_t right_error;

		CHECK(v34_timing_filter_observe(
			      &left, input, &left_error) ==
			      V34_TIMING_FILTER_OK &&
		      v34_timing_filter_observe(
			      &right, input, &right_error) ==
			      V34_TIMING_FILTER_OK &&
		      left_error == right_error,
		      "V.34 timing owners diverged at observation %u", index);
		if ((index % 7u) == 1u || (index % 7u) == 3u ||
		    (index % 7u) == 5u) {
			struct v34_analytic_sample left_output;
			struct v34_analytic_sample right_output;

			CHECK(v34_timing_filter_prefilter(
				      &left, &left_output) ==
				      V34_TIMING_FILTER_OK &&
			      v34_timing_filter_prefilter(
				      &right, &right_output) ==
				      V34_TIMING_FILTER_OK &&
			      memcmp(&left_output, &right_output,
				     sizeof(left_output)) == 0,
			      "V.34 timing prefilters diverged at %u", index);
		}
		CHECK(memcmp(&left, &right, sizeof(left)) == 0,
		      "V.34 timing owner state diverged at %u", index);
	}
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "exact filter", test_exact_filter },
		{ "ownership", test_ownership },
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
