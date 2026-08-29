// SPDX-License-Identifier: BSD-3-Clause

#include <v34_16_state_branches.h>

#include "test.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
	ORACLE_INTERVALS = 4096,
};

static uint32_t next_word(uint32_t *state)
{
	*state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
	return *state;
}

static uint64_t hash_byte(uint64_t hash, uint8_t value)
{
	return (hash ^ value) * UINT64_C(1099511628211);
}

static uint64_t hash_u16(uint64_t hash, uint16_t value)
{
	hash = hash_byte(hash, (uint8_t)value);
	return hash_byte(hash, (uint8_t)(value >> 8u));
}

static uint64_t hash_profile(
	uint64_t hash, const struct v34_16_state_branch_profile *profile)
{
	hash = hash_byte(hash, (uint8_t)profile->symbol_rate);
	hash = hash_byte(
		hash, profile->primary_data_rate_2400_bps_units);
	hash = hash_byte(hash, profile->expanded_shaping);
	hash = hash_u16(hash, profile->metric);
	return hash_byte(hash, profile->distance_shift);
}

static uint64_t hash_branch(
	uint64_t hash, const struct v34_16_state_branch *branch)
{
	for (uint8_t symbol = 0;
	     symbol < V34_2D_SYMBOLS_PER_4D_INTERVAL; ++symbol) {
		hash = hash_u16(
			hash, (uint16_t)branch->decision
				.channel_output[symbol].in_phase);
		hash = hash_u16(
			hash, (uint16_t)branch->decision
				.channel_output[symbol].quadrature);
	}
	hash = hash_byte(hash, branch->decision.convolutional_output);
	hash = hash_byte(hash, branch->decision.inversion_bit);
	return hash_u16(hash, branch->metric);
}

static bool profiles_are_equal(
	const struct v34_16_state_branch_profile *left,
	const struct v34_16_state_branch_profile *right)
{
	return left->symbol_rate == right->symbol_rate &&
	       left->metric == right->metric &&
	       left->primary_data_rate_2400_bps_units ==
		       right->primary_data_rate_2400_bps_units &&
	       left->distance_shift == right->distance_shift &&
	       left->expanded_shaping == right->expanded_shaping;
}

static bool test_profiles(void)
{
	uint64_t hash = UINT64_C(14695981039346656037);
	unsigned count = 0;

	for (enum v34_symbol_rate symbol_rate = V34_SYMBOL_RATE_2400;
	     symbol_rate < V34_SYMBOL_RATE_COUNT; ++symbol_rate) {
		for (uint8_t rate = 1;
		     rate <= V34_INFO1_MAX_DATA_RATE_2400_BPS_UNITS; ++rate) {
			struct v34_frame_geometry geometry;

			if (v34_frame_geometry_init(
				    &geometry, symbol_rate, rate, false) !=
			    V34_FRAME_SCHEDULE_OK)
				continue;
			for (uint8_t expanded = 0; expanded < 2; ++expanded) {
				struct v34_16_state_branch_profile profile;
				struct v34_16_state_branch_profile auxiliary;
				struct v34_frame_geometry auxiliary_geometry;

				CHECK(v34_16_state_branch_profile_init(
					      &profile, &geometry, expanded != 0) ==
					      V34_16_STATE_BRANCHES_OK &&
				      v34_16_state_branch_profile_is_valid(
					      &profile),
				      "profile %u/%u/%u did not initialize",
				      symbol_rate, rate, expanded);
				CHECK(v34_frame_geometry_init(
					      &auxiliary_geometry, symbol_rate, rate,
					      true) == V34_FRAME_SCHEDULE_OK &&
				      v34_16_state_branch_profile_init(
					      &auxiliary, &auxiliary_geometry,
					      expanded != 0) ==
					      V34_16_STATE_BRANCHES_OK &&
				      profiles_are_equal(&profile, &auxiliary),
				      "auxiliary channel changed profile %u/%u/%u",
				      symbol_rate, rate, expanded);
				hash = hash_profile(hash, &profile);
				count++;
			}
		}
	}
	CHECK(count == 130 && hash == UINT64_C(0x4c13615f30476d84),
	      "profile oracle changed: %u/%016llx", count,
	      (unsigned long long)hash);

	struct v34_frame_geometry geometry;
	struct v34_16_state_branch_profile ordinary;
	struct v34_16_state_branch_profile expanded;
	CHECK(v34_frame_geometry_init(
		      &geometry, V34_SYMBOL_RATE_3429, 10, false) ==
		      V34_FRAME_SCHEDULE_OK &&
	      v34_16_state_branch_profile_init(
		      &ordinary, &geometry, false) ==
		      V34_16_STATE_BRANCHES_OK &&
	      v34_16_state_branch_profile_init(
		      &expanded, &geometry, true) ==
		      V34_16_STATE_BRANCHES_OK &&
	      ordinary.metric == 313 && ordinary.distance_shift == 10 &&
	      expanded.metric == 322 && expanded.distance_shift == 10,
	      "3429/24000 profile changed");
	CHECK(v34_frame_geometry_init(
		      &geometry, V34_SYMBOL_RATE_2743, 11, false) ==
		      V34_FRAME_SCHEDULE_OK &&
	      v34_16_state_branch_profile_init(
		      &ordinary, &geometry, false) ==
		      V34_16_STATE_BRANCHES_OK &&
	      ordinary.metric == 133 && ordinary.distance_shift == 9,
	      "2743 shared profile changed");
	return true;
}

static bool test_branch_oracle(void)
{
	struct v34_frame_geometry geometry;
	struct v34_16_state_branch_profile profile;
	struct v34_16_state_survivor survivor;
	uint32_t random = UINT32_C(0x9e3779b9);
	uint64_t hash = UINT64_C(14695981039346656037);
	unsigned traceback_count = 0;

	CHECK(v34_frame_geometry_init(
		      &geometry, V34_SYMBOL_RATE_3429, 10, false) ==
		      V34_FRAME_SCHEDULE_OK &&
	      v34_16_state_branch_profile_init(
		      &profile, &geometry, false) ==
		      V34_16_STATE_BRANCHES_OK &&
	      v34_16_state_survivor_init(&survivor) ==
		      V34_16_STATE_SURVIVOR_OK,
	      "oracle owners did not initialize");
	for (unsigned interval = 0; interval < ORACLE_INTERVALS;
	     ++interval) {
		struct v34_analytic_sample samples[2];
		struct v34_16_state_branch branches[V34_16_STATE_BRANCH_COUNT];
		struct v34_survivor_decision current;
		struct v34_survivor_decision traceback;
		bool ready;
		uint8_t inversion = (uint8_t)(next_word(&random) & 1u);

		for (uint8_t symbol = 0; symbol < 2; ++symbol) {
			samples[symbol].in_phase = (int16_t)next_word(&random);
			samples[symbol].quadrature = (int16_t)next_word(&random);
		}
		CHECK(v34_16_state_branches_build(
			      &profile, samples, inversion, branches) ==
			      V34_16_STATE_BRANCHES_OK,
		      "branch oracle failed at interval %u", interval);
		for (uint8_t branch = 0;
		     branch < V34_16_STATE_BRANCH_COUNT; ++branch)
			hash = hash_branch(hash, &branches[branch]);
		CHECK(v34_16_state_survivor_advance(
			      &survivor, branches, &current, &traceback,
			      &ready) == V34_16_STATE_SURVIVOR_OK,
		      "branch composition failed at interval %u", interval);
		traceback_count += ready;
	}
	CHECK(hash == UINT64_C(0x111e9c67309a65d7),
	      "fixed branch oracle changed: %016llx",
	      (unsigned long long)hash);
	CHECK(traceback_count ==
		      ORACLE_INTERVALS -
		      V34_16_STATE_TRACEBACK_INTERVALS + 1u,
	      "survivor composition traceback count changed");
	return true;
}

static bool test_quantization(void)
{
	struct v34_frame_geometry geometry;
	struct v34_16_state_branch_profile profile;
	struct qam_point point = { 1.5, -2.5 };
	struct v34_analytic_sample fixed = { 17, 19 };
	struct v34_analytic_sample saved;

	CHECK(v34_frame_geometry_init(
		      &geometry, V34_SYMBOL_RATE_3429, 10, false) ==
		      V34_FRAME_SCHEDULE_OK &&
	      v34_16_state_branch_profile_init(
		      &profile, &geometry, false) ==
		      V34_16_STATE_BRANCHES_OK &&
	      v34_16_state_branch_quantize(&profile, &point, &fixed) ==
		      V34_16_STATE_BRANCHES_OK &&
	      fixed.in_phase == 470 && fixed.quadrature == -783,
	      "normalized quantization changed");
	saved = fixed;
	point.in_phase = NAN;
	CHECK(v34_16_state_branch_quantize(&profile, &point, &fixed) ==
		      V34_16_STATE_BRANCHES_INVALID_ARGUMENT &&
	      memcmp(&fixed, &saved, sizeof(fixed)) == 0,
	      "nonfinite quantization mutated output");
	point = (struct qam_point){ 40000.0 / profile.metric, 0.0 };
	CHECK(v34_16_state_branch_quantize(&profile, &point, &fixed) ==
		      V34_16_STATE_BRANCHES_OUT_OF_RANGE &&
	      memcmp(&fixed, &saved, sizeof(fixed)) == 0,
	      "out-of-range quantization mutated output");
	point = (struct qam_point){ 1.0, -1.0 };
	struct qam_point saved_point = point;
	CHECK(v34_16_state_branch_quantize(
		      &profile, &point,
		      (struct v34_analytic_sample *)(void *)&point) ==
		      V34_16_STATE_BRANCHES_INVALID_ARGUMENT &&
	      memcmp(&point, &saved_point, sizeof(point)) == 0,
	      "overlapping quantization mutated input");
	return true;
}

static bool rejected_build(
	const struct v34_16_state_branch_profile *profile,
	const struct v34_analytic_sample samples[2], uint8_t inversion,
	enum v34_16_state_branches_result expected)
{
	struct v34_16_state_branch branches[V34_16_STATE_BRANCH_COUNT];
	struct v34_16_state_branch saved[V34_16_STATE_BRANCH_COUNT];

	memset(branches, 0xa5, sizeof(branches));
	memcpy(saved, branches, sizeof(saved));
	return v34_16_state_branches_build(
		       profile, samples, inversion, branches) == expected &&
	       memcmp(branches, saved, sizeof(saved)) == 0;
}

static bool test_transactions(void)
{
	struct v34_frame_geometry geometry;
	struct v34_16_state_branch_profile profile = { 0 };
	struct v34_16_state_branch_profile saved_profile;
	struct v34_analytic_sample samples[2] = {
		{ 1234, -5678 },
		{ -9012, 3456 },
	};
	struct v34_16_state_branch branches[V34_16_STATE_BRANCH_COUNT];
	struct v34_16_state_branch repeated[V34_16_STATE_BRANCH_COUNT];

	memset(&profile, 0xa5, sizeof(profile));
	saved_profile = profile;
	memset(&geometry, 0, sizeof(geometry));
	CHECK(v34_16_state_branch_profile_init(
		      &profile, &geometry, false) ==
		      V34_16_STATE_BRANCHES_INVALID_ARGUMENT &&
	      memcmp(&profile, &saved_profile, sizeof(profile)) == 0,
	      "invalid geometry mutated profile");
	CHECK(v34_frame_geometry_init(
		      &geometry, V34_SYMBOL_RATE_3429, 10, false) ==
		      V34_FRAME_SCHEDULE_OK &&
	      v34_16_state_branch_profile_init(
		      &profile, &geometry, false) ==
		      V34_16_STATE_BRANCHES_OK,
	      "transaction profile did not initialize");
	CHECK(v34_16_state_branches_build(
		      &profile, samples, 1, branches) ==
		      V34_16_STATE_BRANCHES_OK &&
	      v34_16_state_branches_build(
		      &profile, samples, 1, repeated) ==
		      V34_16_STATE_BRANCHES_OK &&
	      memcmp(branches, repeated, sizeof(branches)) == 0,
	      "branch construction is not reentrant");
	CHECK(rejected_build(
		      &profile, samples, 2,
		      V34_16_STATE_BRANCHES_INVALID_ARGUMENT),
	      "invalid inversion mutated branches");
	struct v34_16_state_branch_profile corrupt = profile;
	corrupt.distance_shift++;
	CHECK(!v34_16_state_branch_profile_is_valid(&corrupt) &&
	      rejected_build(
		      &corrupt, samples, 0,
		      V34_16_STATE_BRANCHES_INVALID_ARGUMENT),
	      "corrupt profile was accepted");
	union branch_alias {
		struct v34_analytic_sample samples[2];
		struct v34_16_state_branch
			branches[V34_16_STATE_BRANCH_COUNT];
	} alias = { .samples = {
		{ 1234, -5678 },
		{ -9012, 3456 },
	} };
	union branch_alias saved_alias = alias;
	CHECK(v34_16_state_branches_build(
		      &profile, alias.samples, 0, alias.branches) ==
		      V34_16_STATE_BRANCHES_INVALID_ARGUMENT &&
	      memcmp(&alias, &saved_alias, sizeof(alias)) == 0,
	      "overlapping branch output mutated input");
	CHECK(v34_frame_geometry_init(
		      &geometry, V34_SYMBOL_RATE_3429, 14, false) ==
		      V34_FRAME_SCHEDULE_OK &&
	      v34_16_state_branch_profile_init(
		      &profile, &geometry, false) ==
		      V34_16_STATE_BRANCHES_OK,
	      "small-metric profile did not initialize");
	samples[0] = (struct v34_analytic_sample){ INT16_MAX, INT16_MAX };
	samples[1] = samples[0];
	CHECK(rejected_build(
		      &profile, samples, 0,
		      V34_16_STATE_BRANCHES_OUT_OF_RANGE),
	      "unbounded lattice output mutated branches");
	CHECK(!v34_16_state_branch_profile_is_valid(NULL) &&
	      v34_16_state_branches_build(NULL, samples, 0, branches) ==
		      V34_16_STATE_BRANCHES_INVALID_ARGUMENT &&
	      v34_16_state_branches_build(
		      &profile, NULL, 0, branches) ==
		      V34_16_STATE_BRANCHES_INVALID_ARGUMENT &&
	      v34_16_state_branches_build(
		      &profile, samples, 0, NULL) ==
		      V34_16_STATE_BRANCHES_INVALID_ARGUMENT,
	      "null contract changed");
	return true;
}

int main(void)
{
	if (!test_profiles() || !test_branch_oracle() ||
	    !test_quantization() || !test_transactions())
		return 1;
	puts("V.34 16-state branch tests passed");
	return 0;
}
