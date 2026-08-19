// SPDX-License-Identifier: BSD-3-Clause

#include <trellis_profile.h>

#include <constellation.h>

#include "test.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
	TEST_BOUNDARY_COUNT = 101,
	TEST_RANDOM_POINTS = 4096,
};

struct profile_fixture {
	uint8_t data_bits;
	uint8_t window;
	size_t label_count;
	size_t region_count;
	uint64_t profile_digest;
	uint64_t candidate_digest;
};

static const struct profile_fixture FIXTURES[] = {
	{ 3, 2, 128, 8, UINT64_C(0x294d14b800c3536f),
	  UINT64_C(0xd6f4cdb98073cb64) },
	{ 4, 4, 416, 32, UINT64_C(0x19151c04b64f6bea),
	  UINT64_C(0x262ee9c8128a48e8) },
	{ 5, 6, 960, 72, UINT64_C(0x4a8551ec8d981b8b),
	  UINT64_C(0xa7cf0f33ba670e9d) },
	{ 6, 8, 1856, 128, UINT64_C(0xb4fbc5f6b0e5ab8f),
	  UINT64_C(0x2a345e18d3a97c66) },
};

static uint64_t hash_byte(uint64_t hash, uint8_t value)
{
	return (hash ^ value) * UINT64_C(1099511628211);
}

static uint64_t profile_digest(const struct v32_trellis_profile *profile)
{
	uint64_t hash = UINT64_C(14695981039346656037);

	for (size_t index = 0; index < profile->candidate_label_count; ++index)
		hash = hash_byte(hash, profile->candidate_labels[index]);
	for (size_t index = 0; index < profile->region_block_count; ++index)
		hash = hash_byte(hash, profile->region_blocks[index]);
	return hash;
}

static void add_boundary(
	int16_t values[TEST_BOUNDARY_COUNT], size_t *count, int value)
{
	if (value < INT16_MIN || value > INT16_MAX)
		return;
	for (size_t index = 0; index < *count; ++index) {
		if (values[index] == value)
			return;
	}
	if (*count < TEST_BOUNDARY_COUNT)
		values[(*count)++] = (int16_t)value;
}

static size_t boundary_grid(int16_t values[TEST_BOUNDARY_COUNT])
{
	size_t count = 0;

	add_boundary(values, &count, INT16_MIN);
	add_boundary(values, &count, INT16_MAX);
	for (unsigned base_index = 0; base_index < 17; ++base_index) {
		int base = base_index == 0 ? 0 :
			   0x400 + 0x800 * (int)(base_index - 1u);

		for (int sign = -1; sign <= 1; sign += 2) {
			for (int delta = -1; delta <= 1; ++delta)
				add_boundary(
					values, &count, sign * base + delta);
		}
	}
	return count;
}

static uint32_t fixture_random(uint32_t *state)
{
	*state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
	return *state;
}

static int16_t high_word_s16(uint32_t value)
{
	uint16_t word = (uint16_t)(value >> 16);

	return word <= INT16_MAX ? (int16_t)word :
	       (int16_t)((int32_t)word - 65536);
}

static bool add_candidates(
	const struct v32_trellis_profile *profile, int16_t in_phase,
	int16_t quadrature, uint64_t *digest)
{
	uint8_t candidates[V32_TRELLIS_PROFILE_CANDIDATES];

	CHECK(v32_trellis_profile_candidates(
		      profile, in_phase, quadrature, candidates) ==
		      V32_TRELLIS_PROFILE_OK,
	      "%u-bit profile rejected %d%+di",
	      profile->data_bits_per_symbol, in_phase, quadrature);
	for (size_t index = 0; index < ARRAY_LENGTH(candidates); ++index)
		*digest = hash_byte(*digest, candidates[index]);
	return true;
}

static bool test_profiles(void)
{
	int16_t boundaries[TEST_BOUNDARY_COUNT];
	size_t boundary_count = boundary_grid(boundaries);

	CHECK(boundary_count == ARRAY_LENGTH(boundaries),
	      "boundary grid has %zu points", boundary_count);
	for (size_t fixture_index = 0;
	     fixture_index < ARRAY_LENGTH(FIXTURES); ++fixture_index) {
		const struct profile_fixture *fixture = &FIXTURES[fixture_index];
		const struct v32_trellis_profile *profile =
			v32_trellis_profile_for_data_bits(fixture->data_bits);
		uint64_t candidates = UINT64_C(14695981039346656037);
		uint32_t random = UINT32_C(0x7b320000) ^
			(UINT32_C(0x9e3779b9) * fixture->data_bits);
		size_t signal_points = (size_t)1u << (fixture->data_bits + 1u);

		CHECK(v32_trellis_profile_is_valid(profile) &&
		      profile->data_bits_per_symbol == fixture->data_bits &&
		      profile->region_window == fixture->window &&
		      profile->candidate_label_count == fixture->label_count &&
		      profile->region_block_count == fixture->region_count &&
		      fixture->region_count ==
			      2u * fixture->window * fixture->window,
		      "%u-bit profile shape changed", fixture->data_bits);
		CHECK(profile_digest(profile) == fixture->profile_digest,
		      "%u-bit compact profile digest changed", fixture->data_bits);
		for (size_t index = 0;
		     index < profile->candidate_label_count; ++index) {
			struct v32_fixed_point point;

			CHECK(profile->candidate_labels[index] < signal_points &&
			      v32_trellis_fixed_signal_point(
				      fixture->data_bits,
				      profile->candidate_labels[index], &point) ==
				      V32_CONSTELLATION_OK,
			      "%u-bit candidate label %zu is invalid",
			      fixture->data_bits, index);
		}
		for (size_t index = 0;
		     index < profile->region_block_count; ++index) {
			size_t last = (size_t)profile->region_blocks[index] *
				V32_TRELLIS_PROFILE_REGION_BLOCK_LABELS +
				V32_TRELLIS_PROFILE_REGION_BLOCK_LABELS - 1u;

			CHECK(last < profile->candidate_label_count,
			      "%u-bit region %zu exceeds the candidate profile",
			      fixture->data_bits, index);
		}
		for (size_t i = 0; i < boundary_count; ++i) {
			for (size_t q = 0; q < boundary_count; ++q) {
				CHECK(add_candidates(
					      profile, boundaries[i], boundaries[q],
					      &candidates),
				      "%u-bit boundary candidates failed",
				      fixture->data_bits);
			}
		}
		for (size_t index = 0; index < TEST_RANDOM_POINTS; ++index) {
			int16_t in_phase = high_word_s16(fixture_random(&random));
			int16_t quadrature = high_word_s16(fixture_random(&random));

			CHECK(add_candidates(
				      profile, in_phase, quadrature, &candidates),
			      "%u-bit random candidates failed", fixture->data_bits);
		}
		CHECK(candidates == fixture->candidate_digest,
		      "%u-bit candidate digest was %016llx", fixture->data_bits,
		      (unsigned long long)candidates);
	}
	return true;
}

static bool test_contracts(void)
{
	const struct v32_trellis_profile *profile =
		v32_trellis_profile_for_data_bits(FIXTURES[0].data_bits);
	struct v32_trellis_profile copied;
	uint8_t candidates[V32_TRELLIS_PROFILE_CANDIDATES];
	uint8_t unchanged[V32_TRELLIS_PROFILE_CANDIDATES];

	memset(candidates, 0xa5, sizeof(candidates));
	memcpy(unchanged, candidates, sizeof(unchanged));
	CHECK(profile != NULL, "valid profile lookup failed");
	copied = *profile;
	CHECK(v32_trellis_profile_for_data_bits(2) == NULL &&
	      v32_trellis_profile_for_data_bits(7) == NULL &&
	      !v32_trellis_profile_is_valid(NULL) &&
	      !v32_trellis_profile_is_valid(&copied),
	      "invalid or copied profile was accepted");
	CHECK(v32_trellis_profile_candidates(NULL, 0, 0, candidates) ==
		      V32_TRELLIS_PROFILE_INVALID_ARGUMENT &&
	      memcmp(candidates, unchanged, sizeof(candidates)) == 0 &&
	      v32_trellis_profile_candidates(&copied, 0, 0, candidates) ==
		      V32_TRELLIS_PROFILE_INVALID_ARGUMENT &&
	      memcmp(candidates, unchanged, sizeof(candidates)) == 0 &&
	      v32_trellis_profile_candidates(profile, 0, 0, NULL) ==
		      V32_TRELLIS_PROFILE_INVALID_ARGUMENT,
	      "profile rejection mutated output");
	CHECK(v32_trellis_profile_candidates(
		      profile, 0, 0, (uint8_t *)(uintptr_t)profile) ==
		      V32_TRELLIS_PROFILE_INVALID_ARGUMENT &&
	      v32_trellis_profile_candidates(
		      profile, 0, 0,
		      (uint8_t *)(uintptr_t)profile->candidate_labels) ==
		      V32_TRELLIS_PROFILE_INVALID_ARGUMENT &&
	      v32_trellis_profile_candidates(
		      profile, 0, 0,
		      (uint8_t *)(uintptr_t)profile->region_blocks) ==
		      V32_TRELLIS_PROFILE_INVALID_ARGUMENT,
	      "profile storage was accepted as mutable output");
	CHECK(v32_trellis_profile_candidates(profile, 0, 0, candidates) ==
		      V32_TRELLIS_PROFILE_OK &&
	      memcmp(candidates, unchanged, sizeof(candidates)) != 0,
	      "valid candidate lookup did not publish output");
	return true;
}

int main(void)
{
	if (!test_profiles() || !test_contracts())
		return 1;
	puts("V.32 trellis profile tests passed");
	return 0;
}
