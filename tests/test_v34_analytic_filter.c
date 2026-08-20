// SPDX-License-Identifier: BSD-3-Clause

#include <v34_analytic_filter.h>

#include "test.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const double PI = 3.1415926535897932384626433832795;

static uint64_t hash_word(uint64_t hash, uint16_t word)
{
	for (unsigned int byte = 0; byte < 2; ++byte) {
		hash ^= (word >> (byte * 8u)) & 0xffu;
		hash *= UINT64_C(1099511628211);
	}
	return hash;
}

static bool test_reference_profiles(void)
{
	static const struct {
		unsigned int index;
		int16_t in_phase;
		int16_t quadrature;
	} impulse_points[] = {
		{ 0, -21, 0 }, { 1, 0, 116 }, { 2, -53, 0 },
		{ 3, 0, 137 }, { 4, -97, 0 }, { 5, 0, 151 },
		{ 6, -154, 0 }, { 7, 0, 154 }, { 28, -1013, 0 },
		{ 29, 0, -10369 }, { 30, 15360, 0 },
		{ 31, 0, 10369 }, { 32, -1013, 0 }, { 33, 0, 3295 },
	};
	struct v34_analytic_filter filter;
	uint64_t impulse_hash = UINT64_C(1469598103934665603);
	uint64_t random_hash = UINT64_C(1469598103934665603);
	uint32_t random = UINT32_C(0x91e10da5);
	size_t checked = 0;

	CHECK(v34_analytic_filter_init(&filter) ==
		      V34_ANALYTIC_FILTER_OK,
	      "impulse owner did not initialize");
	for (unsigned int index = 0; index < 128; ++index) {
		struct v34_analytic_sample output;

		CHECK(v34_analytic_filter_process(
			      &filter, index == 0 ? 16384 : 0, &output) ==
			      V34_ANALYTIC_FILTER_OK,
		      "impulse sample %u failed", index);
		impulse_hash = hash_word(impulse_hash, (uint16_t)output.in_phase);
		impulse_hash = hash_word(
			impulse_hash, (uint16_t)output.quadrature);
		if (checked < ARRAY_LENGTH(impulse_points) &&
		    impulse_points[checked].index == index) {
			CHECK(output.in_phase == impulse_points[checked].in_phase &&
				      output.quadrature ==
					      impulse_points[checked].quadrature,
			      "impulse sample %u changed: %d/%d", index,
			      output.in_phase, output.quadrature);
			checked++;
		}
	}
	CHECK(checked == ARRAY_LENGTH(impulse_points) &&
		      impulse_hash == UINT64_C(0x013ed3b6fe5034b6),
	      "analytic impulse profile changed: %#llx",
	      (unsigned long long)impulse_hash);

	CHECK(v34_analytic_filter_init(&filter) ==
		      V34_ANALYTIC_FILTER_OK,
	      "random owner did not initialize");
	for (unsigned int index = 0; index < 4096; ++index) {
		struct v34_analytic_sample output;

		random = random * UINT32_C(1664525) + UINT32_C(1013904223);
		CHECK(v34_analytic_filter_process(
			      &filter, (int16_t)(random >> 16), &output) ==
			      V34_ANALYTIC_FILTER_OK,
		      "random sample %u failed", index);
		random_hash = hash_word(random_hash, (uint16_t)output.in_phase);
		random_hash = hash_word(
			random_hash, (uint16_t)output.quadrature);
	}
	for (size_t index = 0; index < V34_ANALYTIC_FILTER_TAPS; ++index)
		random_hash = hash_word(
			random_hash, (uint16_t)filter.history[index]);
	CHECK(random == UINT32_C(0x38409da5) &&
		      random_hash == UINT64_C(0x5b81dfa0f803dbdf),
	      "analytic random profile changed: %#x/%#llx", random,
	      (unsigned long long)random_hash);
	return true;
}

static bool tone_energy(unsigned int frequency, uint64_t *energy)
{
	struct v34_analytic_filter filter;
	uint64_t accumulated = 0;

	if (v34_analytic_filter_init(&filter) != V34_ANALYTIC_FILTER_OK)
		return false;
	for (unsigned int index = 0; index < 4096; ++index) {
		struct v34_analytic_sample output;
		int16_t sample = (int16_t)lround(
			12000.0 * cos(2.0 * PI * frequency * index / 8000.0));

		if (v34_analytic_filter_process(&filter, sample, &output) !=
		    V34_ANALYTIC_FILTER_OK)
			return false;
		if (index >= 128)
			accumulated +=
				(uint64_t)((int32_t)output.in_phase *
					   output.in_phase) +
				(uint64_t)((int32_t)output.quadrature *
					   output.quadrature);
	}
	*energy = accumulated;
	return true;
}

static bool test_voice_band(void)
{
	uint64_t stop_low;
	uint64_t edge_low;
	uint64_t middle;
	uint64_t edge_high;
	uint64_t stop_high;

	CHECK(tone_energy(100, &stop_low) &&
		      tone_energy(300, &edge_low) &&
		      tone_energy(1800, &middle) &&
		      tone_energy(3700, &edge_high) &&
		      tone_energy(3900, &stop_high),
	      "analytic band measurement failed");
	CHECK(edge_low > 7u * stop_low && edge_high > 7u * stop_high &&
		      middle > 7u * stop_low && middle > 7u * stop_high,
	      "analytic passband changed: %llu/%llu/%llu/%llu/%llu",
	      (unsigned long long)stop_low,
	      (unsigned long long)edge_low,
	      (unsigned long long)middle,
	      (unsigned long long)edge_high,
	      (unsigned long long)stop_high);
	return true;
}

static bool test_ownership(void)
{
	struct v34_analytic_filter left;
	struct v34_analytic_filter right;
	struct v34_analytic_filter saved;
	struct v34_analytic_filter uninitialized = { 0 };
	struct v34_analytic_sample output = { 123, -456 };
	struct v34_analytic_sample saved_output = output;
	uint32_t random = UINT32_C(0x24c8719b);

	CHECK(v34_analytic_filter_init(&left) ==
		      V34_ANALYTIC_FILTER_OK &&
		      v34_analytic_filter_init(&right) ==
			      V34_ANALYTIC_FILTER_OK &&
		      !v34_analytic_filter_is_valid(NULL) &&
		      v34_analytic_filter_init(NULL) ==
			      V34_ANALYTIC_FILTER_INVALID_ARGUMENT,
	      "analytic lifecycle contract changed");
	saved = left;
	CHECK(v34_analytic_filter_process(&uninitialized, 0, &output) ==
		      V34_ANALYTIC_FILTER_INVALID_STATE &&
		      v34_analytic_filter_process(NULL, 0, &output) ==
		      V34_ANALYTIC_FILTER_INVALID_STATE &&
		      v34_analytic_filter_process(&left, 0, NULL) ==
			      V34_ANALYTIC_FILTER_INVALID_ARGUMENT &&
		      v34_analytic_filter_process(
			      &left, 0,
			      (struct v34_analytic_sample *)&left) ==
			      V34_ANALYTIC_FILTER_INVALID_ARGUMENT &&
		      memcmp(&left, &saved, sizeof(left)) == 0 &&
		      memcmp(&output, &saved_output, sizeof(output)) == 0,
	      "analytic rejection was not transactional");

	for (unsigned int index = 0; index < 1024; ++index) {
		struct v34_analytic_sample left_output;
		struct v34_analytic_sample right_output;
		int16_t sample;

		random = random * UINT32_C(1103515245) + UINT32_C(12345);
		sample = (int16_t)(random >> 16);
		CHECK(v34_analytic_filter_process(
			      &left, sample, &left_output) ==
			      V34_ANALYTIC_FILTER_OK &&
			      v34_analytic_filter_process(
				      &right, sample, &right_output) ==
				      V34_ANALYTIC_FILTER_OK &&
			      memcmp(&left_output, &right_output,
				     sizeof(left_output)) == 0 &&
			      memcmp(&left, &right, sizeof(left)) == 0,
		      "analytic owners diverged at sample %u", index);
	}
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "reference profiles", test_reference_profiles },
		{ "voice band", test_voice_band },
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
