// SPDX-License-Identifier: BSD-3-Clause

#include <v34_16_state_branches.h>

#include "test.h"

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

static bool initialize_profile(
	struct v34_16_state_branch_profile *profile)
{
	struct v34_frame_geometry geometry;

	return v34_frame_geometry_init(
		       &geometry, V34_SYMBOL_RATE_3429, 10, false) ==
		       V34_FRAME_SCHEDULE_OK &&
	       v34_16_state_branch_profile_init(
		       profile, &geometry, false) ==
		       V34_16_STATE_BRANCHES_OK;
}

static bool generated_branches(
	uint32_t *random, const struct v34_16_state_branch_profile *profile,
	struct v34_16_state_branch
		branches[V34_16_STATE_BRANCH_COUNT])
{
	struct v34_analytic_sample samples[V34_2D_SYMBOLS_PER_4D_INTERVAL];
	uint8_t inversion = (uint8_t)(next_word(random) & 1u);

	for (uint8_t symbol = 0;
	     symbol < V34_2D_SYMBOLS_PER_4D_INTERVAL; ++symbol) {
		int in_phase = (int)((next_word(random) >> 16) % 31u) - 15;
		int quadrature = (int)((next_word(random) >> 16) % 31u) - 15;
		struct qam_point estimate = {
			.in_phase = in_phase +
				(double)(next_word(random) & 0xffffu) / 65536.0,
			.quadrature = quadrature +
				(double)(next_word(random) & 0xffffu) / 65536.0,
		};

		if (v34_16_state_branch_quantize(
			    profile, &estimate, &samples[symbol]) !=
		    V34_16_STATE_BRANCHES_OK)
			return false;
	}
	if (v34_16_state_branches_build(
		    profile, samples, inversion, branches) !=
	    V34_16_STATE_BRANCHES_OK)
		return false;
	for (uint8_t branch = 0; branch < V34_16_STATE_BRANCH_COUNT;
	     ++branch)
		branches[branch].metric = (uint16_t)next_word(random);
	return true;
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

static uint64_t hash_decision(
	uint64_t hash, const struct v34_survivor_decision *decision)
{
	for (uint8_t symbol = 0;
	     symbol < V34_2D_SYMBOLS_PER_4D_INTERVAL; ++symbol) {
		hash = hash_u16(
			hash,
			(uint16_t)decision->channel_output[symbol].in_phase);
		hash = hash_u16(
			hash,
			(uint16_t)decision->channel_output[symbol].quadrature);
	}
	hash = hash_byte(hash, decision->convolutional_output);
	return hash_byte(hash, decision->inversion_bit);
}

static uint64_t hash_state(
	const struct v34_16_state_survivor *survivor)
{
	uint64_t hash = UINT64_C(14695981039346656037);

	for (uint8_t state = 0; state < V34_16_STATE_COUNT; ++state)
		hash = hash_u16(hash, survivor->score[state]);
	for (uint8_t interval = 0;
	     interval < V34_16_STATE_TRACEBACK_INTERVALS; ++interval) {
		for (uint8_t state = 0; state < V34_16_STATE_COUNT; ++state) {
			hash = hash_byte(
				hash, survivor->path[interval].predecessor[state]);
			hash = hash_decision(
				hash,
				&survivor->path[interval].decision[state]);
		}
	}
	hash = hash_byte(hash, survivor->path_index);
	return hash_byte(hash, survivor->path_intervals);
}

static bool test_exact_oracle(void)
{
	struct v34_16_state_branch_profile profile;
	struct v34_16_state_survivor survivor;
	uint32_t random = UINT32_C(0x6ca392d1);
	uint64_t current_hash = UINT64_C(14695981039346656037);
	uint64_t traceback_hash = UINT64_C(14695981039346656037);
	unsigned ready_count = 0;

	CHECK(initialize_profile(&profile) &&
	      v34_16_state_survivor_init(&survivor) ==
		      V34_16_STATE_SURVIVOR_OK &&
	      v34_16_state_survivor_is_valid(&survivor),
	      "16-state survivor did not initialize");
	for (unsigned interval = 0; interval < ORACLE_INTERVALS; ++interval) {
		struct v34_16_state_branch branches[V34_16_STATE_BRANCH_COUNT];
		struct v34_survivor_decision current;
		struct v34_survivor_decision traceback;
		struct v34_survivor_decision saved_traceback;
		bool ready = true;

		memset(&traceback, 0xa5, sizeof(traceback));
		saved_traceback = traceback;
		CHECK(generated_branches(&random, &profile, branches) &&
		      v34_16_state_survivor_advance(
			      &survivor, branches, &current, &traceback,
			      &ready) == V34_16_STATE_SURVIVOR_OK,
		      "oracle interval %u failed", interval);
		current_hash = hash_decision(current_hash, &current);
		if (ready) {
			ready_count++;
			traceback_hash = hash_decision(traceback_hash, &traceback);
		} else {
			CHECK(memcmp(
				      &traceback, &saved_traceback,
				      sizeof(traceback)) == 0,
			      "traceback emitted early at %u", interval);
		}
		CHECK(ready ==
			      (interval + 1u >=
			       V34_16_STATE_TRACEBACK_INTERVALS),
		      "traceback readiness changed at %u", interval);
	}
	CHECK(ready_count ==
		      ORACLE_INTERVALS - V34_16_STATE_TRACEBACK_INTERVALS + 1u,
	      "traceback count changed");
	CHECK(current_hash == UINT64_C(0x6bf2fed58941c079) &&
	      traceback_hash == UINT64_C(0xd192dc2a15d910d4) &&
	      hash_state(&survivor) == UINT64_C(0x5f0f4252e39c6b0b),
	      "fixed survivor oracle changed: %016llx/%016llx/%016llx",
	      (unsigned long long)current_hash,
	      (unsigned long long)traceback_hash,
	      (unsigned long long)hash_state(&survivor));
	return true;
}

static bool rejected_advance(
	struct v34_16_state_survivor *survivor,
	struct v34_16_state_branch branches[V34_16_STATE_BRANCH_COUNT],
	enum v34_16_state_survivor_result expected)
{
	struct v34_16_state_survivor saved = { 0 };
	struct v34_survivor_decision current;
	struct v34_survivor_decision traceback;
	struct v34_survivor_decision saved_current;
	struct v34_survivor_decision saved_traceback;
	bool ready = true;

	if (survivor != NULL)
		saved = *survivor;
	memset(&current, 0x5a, sizeof(current));
	memset(&traceback, 0xa5, sizeof(traceback));
	saved_current = current;
	saved_traceback = traceback;
	return v34_16_state_survivor_advance(
		       survivor, branches, &current, &traceback, &ready) ==
		       expected &&
	       (survivor == NULL || memcmp(survivor, &saved, sizeof(saved)) == 0) &&
	       memcmp(&current, &saved_current, sizeof(current)) == 0 &&
	       memcmp(&traceback, &saved_traceback, sizeof(traceback)) == 0 &&
	       ready;
}

static bool test_contracts(void)
{
	struct v34_16_state_branch_profile profile;
	struct v34_16_state_survivor survivor;
	struct v34_16_state_survivor saved;
	struct v34_16_state_survivor corrupted;
	struct v34_16_state_branch branches[V34_16_STATE_BRANCH_COUNT];
	uint32_t random = UINT32_C(0x8ee4d08b);

	memset(&survivor, 0xa5, sizeof(survivor));
	saved = survivor;
	CHECK(v34_16_state_survivor_init(NULL) ==
		      V34_16_STATE_SURVIVOR_INVALID_ARGUMENT &&
	      memcmp(&survivor, &saved, sizeof(saved)) == 0,
	      "null initialization changed state");
	CHECK(initialize_profile(&profile) &&
	      v34_16_state_survivor_init(&survivor) ==
		      V34_16_STATE_SURVIVOR_OK &&
	      generated_branches(&random, &profile, branches),
	      "contract fixture did not initialize");
	CHECK(rejected_advance(
		      NULL, branches, V34_16_STATE_SURVIVOR_INVALID_STATE),
	      "null owner was not transactional");
	CHECK(rejected_advance(
		      &survivor, NULL, V34_16_STATE_SURVIVOR_INVALID_ARGUMENT),
	      "null branches were not transactional");
	{
		struct v34_16_state_branch invalid[V34_16_STATE_BRANCH_COUNT];

		memcpy(invalid, branches, sizeof(invalid));
		invalid[3].decision.convolutional_output ^= 1u;
		CHECK(rejected_advance(
			      &survivor, invalid,
			      V34_16_STATE_SURVIVOR_INVALID_ARGUMENT),
		      "misordered branches changed state");
		memcpy(invalid, branches, sizeof(invalid));
		invalid[5].decision.inversion_bit ^= 1u;
		CHECK(rejected_advance(
			      &survivor, invalid,
			      V34_16_STATE_SURVIVOR_INVALID_ARGUMENT),
		      "mixed inversion changed state");
	}
	corrupted = survivor;
	for (uint8_t state = 0; state < V34_16_STATE_COUNT; ++state)
		corrupted.score[state] = 1;
	CHECK(!v34_16_state_survivor_is_valid(&corrupted) &&
	      rejected_advance(
		      &corrupted, branches,
		      V34_16_STATE_SURVIVOR_INVALID_STATE),
	      "unnormalized owner was accepted");
	corrupted = survivor;
	corrupted.path_index = 1;
	CHECK(!v34_16_state_survivor_is_valid(&corrupted),
	      "misaligned path cursor was accepted");
	{
		struct v34_survivor_decision traceback;
		bool ready;
		struct v34_16_state_survivor before = survivor;

		CHECK(v34_16_state_survivor_advance(
			      &survivor, branches,
			      (struct v34_survivor_decision *)(void *)&survivor,
			      &traceback, &ready) ==
			      V34_16_STATE_SURVIVOR_INVALID_ARGUMENT &&
		      memcmp(&survivor, &before, sizeof(before)) == 0,
		      "owner/output overlap changed state");
	}
	return true;
}

static bool test_reentrancy(void)
{
	struct v34_16_state_branch_profile profile;
	struct v34_16_state_survivor left;
	struct v34_16_state_survivor right;
	uint32_t random = UINT32_C(0xb905703c);

	CHECK(initialize_profile(&profile) &&
	      v34_16_state_survivor_init(&left) ==
		      V34_16_STATE_SURVIVOR_OK,
	      "reentrant owner did not initialize");
	right = left;
	for (unsigned interval = 0; interval < 256; ++interval) {
		struct v34_16_state_branch branches[V34_16_STATE_BRANCH_COUNT];
		struct v34_survivor_decision left_current;
		struct v34_survivor_decision right_current;
		struct v34_survivor_decision left_traceback;
		struct v34_survivor_decision right_traceback;
		bool left_ready;
		bool right_ready;

		CHECK(generated_branches(&random, &profile, branches) &&
		      v34_16_state_survivor_advance(
			      &left, branches, &left_current, &left_traceback,
			      &left_ready) == V34_16_STATE_SURVIVOR_OK &&
		      v34_16_state_survivor_advance(
			      &right, branches, &right_current, &right_traceback,
			      &right_ready) == V34_16_STATE_SURVIVOR_OK &&
		      memcmp(&left, &right, sizeof(left)) == 0 &&
		      memcmp(&left_current, &right_current,
			     sizeof(left_current)) == 0 &&
		      left_ready == right_ready &&
		      (!left_ready || memcmp(
					       &left_traceback, &right_traceback,
					       sizeof(left_traceback)) == 0),
		      "reentrant owners diverged at %u", interval);
	}
	return true;
}

int main(void)
{
	if (!test_exact_oracle() || !test_contracts() || !test_reentrancy())
		return 1;
	puts("V.34 16-state survivor tests passed");
	return 0;
}
