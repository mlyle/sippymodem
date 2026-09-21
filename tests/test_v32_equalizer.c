// SPDX-License-Identifier: BSD-3-Clause

#include <equalizer.h>

#include "test.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
	ORACLE_DECISIONS = 96,
	EQUIVALENCE_DECISIONS = 32,
};

static uint64_t hash_word(uint64_t hash, uint16_t word)
{
	return (hash ^ word) * UINT64_C(1099511628211);
}

static uint64_t hash_u32(uint64_t hash, uint32_t value)
{
	hash = hash_word(hash, (uint16_t)value);
	return hash_word(hash, (uint16_t)(value >> 16));
}

static uint64_t coefficient_hash(const struct v32_equalizer *equalizer)
{
	uint64_t hash = UINT64_C(14695981039346656037);

	for (size_t index = 0; index < V32_EQUALIZER_TAPS; ++index)
		hash = hash_word(hash, (uint16_t)equalizer->i_coefficients[index]);
	for (size_t index = 0; index < V32_EQUALIZER_TAPS; ++index)
		hash = hash_word(hash, (uint16_t)equalizer->q_coefficients[index]);
	return hash;
}

static uint16_t adaptation_index(
	enum v32_equalizer_adaptation adaptation)
{
	if (adaptation == V32_EQUALIZER_ADAPTATION_COARSE)
		return 0;
	if (adaptation == V32_EQUALIZER_ADAPTATION_FINE)
		return 1;
	return 2;
}

static uint64_t state_hash(const struct v32_equalizer *equalizer)
{
	uint64_t hash = UINT64_C(14695981039346656037);

	for (size_t index = 0; index < V32_EQUALIZER_TAPS; ++index)
		hash = hash_word(hash, (uint16_t)equalizer->i_coefficients[index]);
	for (size_t index = 0; index < V32_EQUALIZER_TAPS; ++index)
		hash = hash_word(hash, (uint16_t)equalizer->q_coefficients[index]);
	for (size_t index = 0; index < V32_EQUALIZER_TAPS; ++index)
		hash = hash_word(hash, (uint16_t)equalizer->sample_history[index]);
	hash = hash_word(hash, adaptation_index(equalizer->adaptation));
	hash = hash_word(hash, equalizer->carrier_gain_index);
	hash = hash_u32(hash, (uint32_t)equalizer->carrier_integrator);
	hash = hash_word(hash, equalizer->carrier_tracking);
	hash = hash_word(hash, (uint16_t)equalizer->smoothed_carrier_error);
	hash = hash_word(hash, (uint16_t)equalizer->decision_error);
	hash = hash_word(hash, equalizer->history_index);
	hash = hash_u32(hash, (uint32_t)equalizer->carrier_phase_accumulator);
	hash = hash_word(hash, equalizer->carrier_phase_index);
	hash = hash_word(hash, equalizer->carrier_training_symbols);
	return hash_word(hash, equalizer->samples_until_observation);
}

static int16_t next_sample(uint32_t *state)
{
	*state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
	return (int16_t)(uint16_t)(*state >> 8);
}

static uint16_t ideal_phase(size_t decision)
{
	return (uint16_t)((0x268du + decision * 0x1111u) & 0x7fffu);
}

static uint16_t ideal_magnitude(size_t decision)
{
	return (uint16_t)(0x2000u + ((decision * 257u) & 0x1fffu));
}

static bool adapt_on_error(size_t decision)
{
	return decision % 5u != 1u;
}

static struct v32_equalizer_decision make_decision(
	const struct v32_equalizer_observation *observation, size_t index)
{
	return (struct v32_equalizer_decision) {
		.observation = *observation,
		.ideal_phase = ideal_phase(index),
		.ideal_magnitude = ideal_magnitude(index),
		.adapt_on_error = adapt_on_error(index),
	};
}

static uint64_t hash_observation(
	uint64_t hash, const struct v32_equalizer_observation *observation)
{
	hash = hash_word(hash, (uint16_t)observation->i);
	hash = hash_word(hash, (uint16_t)observation->q);
	hash = hash_word(hash, observation->phase);
	return hash_word(hash, observation->magnitude);
}

static bool test_observation_comparison(void)
{
	const struct v32_equalizer_observation zero = { 0 };
	const struct v32_equalizer_observation nonzero[] = {
		{ .i = INT16_MIN }, { .q = INT16_MAX },
		{ .phase = UINT16_MAX }, { .magnitude = UINT16_MAX },
		{ .epoch = UINT64_MAX }, { .lineage = UINT64_MAX },
	};

	CHECK(v32_equalizer_observation_is_zero(&zero) &&
	      v32_equalizer_observations_are_equal(&zero, &zero),
	      "zero observation comparison failed");
	for (size_t left = 0; left < ARRAY_LENGTH(nonzero); ++left) {
		CHECK(!v32_equalizer_observation_is_zero(&nonzero[left]) &&
		      !v32_equalizer_observations_are_equal(&nonzero[left], &zero) &&
		      !v32_equalizer_observations_are_equal(&zero, &nonzero[left]),
		      "observation field %zu was ignored", left);
		for (size_t right = 0; right < ARRAY_LENGTH(nonzero); ++right)
			CHECK(v32_equalizer_observations_are_equal(
				      &nonzero[left], &nonzero[right]) == (left == right),
			      "observation fields %zu/%zu miscompared", left, right);
	}
	return true;
}

static bool test_initial_profile(void)
{
	struct v32_equalizer equalizer;

	CHECK(v32_equalizer_init(&equalizer) == V32_EQUALIZER_OK &&
	      v32_equalizer_is_valid(&equalizer),
	      "equalizer did not initialize");
	CHECK(coefficient_hash(&equalizer) ==
		      UINT64_C(0x38f3a01c591b58ed),
	      "initial equalizer coefficient profile changed");
	for (size_t index = 0; index < V32_EQUALIZER_TAPS; ++index) {
		CHECK((index % 2u != 0 || equalizer.q_coefficients[index] == 0) &&
		      (index % 2u == 0 || equalizer.i_coefficients[index] == 0),
		      "coefficient zero interleave changed at %zu", index);
	}
	for (size_t profile = 0; profile < 52; ++profile)
		CHECK(equalizer.i_coefficients[profile * 2u] ==
		      -equalizer.i_coefficients[(51u - profile) * 2u],
		      "I coefficient antisymmetry changed at %zu", profile);
	for (size_t profile = 0; profile < 51; ++profile)
		CHECK(equalizer.q_coefficients[profile * 2u + 1u] ==
		      equalizer.q_coefficients[(50u - profile) * 2u + 1u],
		      "Q coefficient symmetry changed at %zu", profile);
	return true;
}

static bool test_exact_oracle(void)
{
	struct v32_equalizer equalizer;
	uint32_t generator = UINT32_C(0x4d595df4);
	uint64_t observations = UINT64_C(14695981039346656037);
	uint64_t initial_coefficients;

	CHECK(v32_equalizer_init(&equalizer) == V32_EQUALIZER_OK,
	      "oracle owner did not initialize");
	initial_coefficients = coefficient_hash(&equalizer);
	for (size_t decision_index = 0;
	     decision_index < ORACLE_DECISIONS; ++decision_index) {
		struct v32_equalizer_observation observation;
		struct v32_equalizer_decision decision;
		int16_t samples[3];
		size_t need = decision_index == 0 ? 1 : 3;
		size_t consumed = SIZE_MAX;

		if (decision_index == 24)
			CHECK(v32_equalizer_set_adaptation(
				      &equalizer,
				      V32_EQUALIZER_ADAPTATION_FINE) ==
				      V32_EQUALIZER_OK,
			      "oracle fine adaptation transition failed");
		if (decision_index == 48)
			CHECK(v32_equalizer_set_carrier_tracking(
				      &equalizer, false) == V32_EQUALIZER_OK,
			      "oracle carrier freeze failed");
		if (decision_index == 56)
			CHECK(v32_equalizer_set_carrier_tracking(
				      &equalizer, true) == V32_EQUALIZER_OK,
			      "oracle carrier resume failed");
		if (decision_index == 64)
			CHECK(v32_equalizer_restart_carrier_acquisition(
				      &equalizer) == V32_EQUALIZER_OK,
			      "oracle carrier reacquisition failed");
		for (size_t index = 0; index < need; ++index)
			samples[index] = next_sample(&generator);
		CHECK(v32_equalizer_process(
			      &equalizer, samples, need,
			      &consumed, &observation) ==
			      V32_EQUALIZER_OBSERVATION_READY &&
		      consumed == need && observation.phase <= INT16_MAX &&
		      observation.magnitude <= INT16_MAX,
		      "oracle observation %zu failed", decision_index);
		observations = hash_observation(observations, &observation);
		decision = make_decision(&observation, decision_index);
		CHECK(v32_equalizer_commit(&equalizer, &decision) ==
			      V32_EQUALIZER_OK,
		      "oracle decision %zu failed", decision_index);
		if (decision_index == 0)
			CHECK(coefficient_hash(&equalizer) != initial_coefficients,
			      "first new-error decision did not adapt");
	}
	CHECK(observations == UINT64_C(0x7242a666a4b41478),
	      "equalizer observation oracle changed: %#llx",
	      (unsigned long long)observations);
	CHECK(coefficient_hash(&equalizer) ==
		      UINT64_C(0xf8dba282d0ed5efb) &&
	      state_hash(&equalizer) == UINT64_C(0xf8b9cd7cb8ccba01),
	      "equalizer terminal oracle changed: %#llx/%#llx",
	      (unsigned long long)coefficient_hash(&equalizer),
	      (unsigned long long)state_hash(&equalizer));
	return true;
}

static bool run_fragmented(
	struct v32_equalizer *equalizer,
	const int16_t *samples, size_t count,
	const size_t *offers, size_t offer_count,
	uint64_t *observation_hash)
{
	size_t input = 0;
	size_t decision_index = 0;
	size_t offer_index = 0;

	*observation_hash = UINT64_C(14695981039346656037);
	while (input < count) {
		struct v32_equalizer_observation observation;
		size_t offer = offers[offer_index++ % offer_count];
		size_t consumed;
		enum v32_equalizer_result result;

		if (offer > count - input)
			offer = count - input;
		result = v32_equalizer_process(
			equalizer, &samples[input], offer,
			&consumed, &observation);
		CHECK((result == V32_EQUALIZER_OK ||
		       result == V32_EQUALIZER_OBSERVATION_READY) &&
		      consumed != 0 && consumed <= offer,
		      "fragmented process failed at input %zu", input);
		input += consumed;
		if (result == V32_EQUALIZER_OBSERVATION_READY) {
			struct v32_equalizer_decision decision =
				make_decision(&observation, decision_index++);

			*observation_hash = hash_observation(
				*observation_hash, &observation);
			CHECK(v32_equalizer_commit(equalizer, &decision) ==
				      V32_EQUALIZER_OK,
			      "fragmented commit failed");
		}
	}
	CHECK(decision_index == EQUIVALENCE_DECISIONS,
	      "fragmented stream produced %zu decisions", decision_index);
	return true;
}

static bool test_fragment_equivalence(void)
{
	static const size_t whole[] = { 1024 };
	static const size_t fragmented[] = { 1, 2, 7, 3, 11, 4 };
	struct v32_equalizer first;
	struct v32_equalizer second;
	int16_t samples[1 + 3 * (EQUIVALENCE_DECISIONS - 1)];
	uint32_t generator = UINT32_C(0x19760517);
	uint64_t first_hash;
	uint64_t second_hash;

	for (size_t index = 0; index < ARRAY_LENGTH(samples); ++index)
		samples[index] = next_sample(&generator);
	CHECK(v32_equalizer_init(&first) == V32_EQUALIZER_OK &&
	      v32_equalizer_init(&second) == V32_EQUALIZER_OK &&
	      run_fragmented(
		      &first, samples, ARRAY_LENGTH(samples),
		      whole, ARRAY_LENGTH(whole), &first_hash) &&
	      run_fragmented(
		      &second, samples, ARRAY_LENGTH(samples),
		      fragmented, ARRAY_LENGTH(fragmented), &second_hash) &&
	      first_hash == second_hash &&
	      memcmp(&first, &second, sizeof(first)) == 0,
	      "equalizer fragmentation changed observations or state");
	return true;
}

static bool rejected_commit_is_unchanged(
	struct v32_equalizer *equalizer,
	const struct v32_equalizer_decision *decision)
{
	struct v32_equalizer saved = *equalizer;

	CHECK(v32_equalizer_commit(equalizer, decision) ==
		      V32_EQUALIZER_INVALID_ARGUMENT &&
	      memcmp(equalizer, &saved, sizeof(saved)) == 0,
	      "invalid decision changed its owner");
	return true;
}

static bool observations_equal(
	const struct v32_equalizer_observation *left,
	const struct v32_equalizer_observation *right)
{
	return left->i == right->i && left->q == right->q &&
	       left->phase == right->phase &&
	       left->magnitude == right->magnitude &&
	       left->epoch == right->epoch &&
	       left->lineage == right->lineage;
}

static bool test_lineage_and_pending_contract(void)
{
	struct v32_equalizer first;
	struct v32_equalizer copy;
	struct v32_equalizer divergent;
	struct v32_equalizer saved;
	struct v32_equalizer_observation first_observation;
	struct v32_equalizer_observation copy_observation;
	struct v32_equalizer_observation divergent_observation;
	struct v32_equalizer_decision first_decision;
	struct v32_equalizer_decision divergent_decision;
	int16_t first_sample = 123;
	int16_t different_sample = 124;
	int16_t next_samples[3] = { 7, -9, 11 };
	size_t consumed;

	CHECK(v32_equalizer_init(&first) == V32_EQUALIZER_OK,
	      "lineage owner did not initialize");
	copy = first;
	divergent = first;
	CHECK(v32_equalizer_process(
		      &first, &first_sample, 1, &consumed,
		      &first_observation) == V32_EQUALIZER_OBSERVATION_READY &&
	      v32_equalizer_process(
		      &copy, &first_sample, 1, &consumed,
		      &copy_observation) == V32_EQUALIZER_OBSERVATION_READY &&
	      v32_equalizer_process(
		      &divergent, &different_sample, 1, &consumed,
		      &divergent_observation) ==
		      V32_EQUALIZER_OBSERVATION_READY &&
	      observations_equal(&first_observation, &copy_observation) &&
	      first_observation.lineage != divergent_observation.lineage,
	      "compatible/divergent lineage was not preserved");
	first_decision = make_decision(&first_observation, 0);
	divergent_decision = make_decision(&divergent_observation, 0);
	CHECK(v32_equalizer_commit(&copy, &first_decision) ==
		      V32_EQUALIZER_OK,
	      "byte-identical copied lineage rejected a compatible decision");
	CHECK(rejected_commit_is_unchanged(&divergent, &first_decision),
	      "divergent lineage accepted a foreign decision");
	CHECK(v32_equalizer_commit(&first, &first_decision) ==
		      V32_EQUALIZER_OK &&
	      v32_equalizer_commit(&divergent, &divergent_decision) ==
		      V32_EQUALIZER_OK,
	      "proper lineage decisions failed");
	CHECK(v32_equalizer_process(
		      &first, next_samples, ARRAY_LENGTH(next_samples),
		      &consumed, &first_observation) ==
		      V32_EQUALIZER_OBSERVATION_READY,
	      "second lineage observation failed");
	saved = first;
	consumed = 99;
	copy_observation = (struct v32_equalizer_observation) {
		.i = 101, .q = -202, .phase = 303, .magnitude = 404,
		.epoch = 505, .lineage = 606,
	};
	divergent_observation = copy_observation;
	CHECK(v32_equalizer_process(
		      &first, next_samples, 1, &consumed,
		      &copy_observation) == V32_EQUALIZER_DECISION_PENDING &&
	      consumed == 99 &&
	      observations_equal(&copy_observation, &divergent_observation) &&
	      v32_equalizer_set_adaptation(
		      &first, V32_EQUALIZER_ADAPTATION_FINE) ==
		      V32_EQUALIZER_DECISION_PENDING &&
	      v32_equalizer_set_adaptation(
		      &first, (enum v32_equalizer_adaptation)0x10000) ==
		      V32_EQUALIZER_DECISION_PENDING &&
	      v32_equalizer_set_carrier_tracking(&first, false) ==
		      V32_EQUALIZER_DECISION_PENDING &&
	      v32_equalizer_restart_carrier_acquisition(&first) ==
		      V32_EQUALIZER_DECISION_PENDING &&
	      memcmp(&first, &saved, sizeof(saved)) == 0,
	      "pending decision did not block input/lifecycle transactionally");
	CHECK(rejected_commit_is_unchanged(&first, &first_decision),
	      "stale observation was accepted by a later epoch");
	first_decision = make_decision(&first_observation, 1);
	CHECK(v32_equalizer_commit(&first, NULL) ==
		      V32_EQUALIZER_INVALID_ARGUMENT &&
	      v32_equalizer_commit(
		      &first, (const struct v32_equalizer_decision *)
		      ((const unsigned char *)&first + 1)) ==
		      V32_EQUALIZER_INVALID_ARGUMENT &&
	      memcmp(&first, &saved, sizeof(saved)) == 0,
	      "null/overlapping decision changed pending state");
	for (unsigned int field = 0; field < 6; ++field) {
		struct v32_equalizer_decision tampered = first_decision;

		if (field == 0)
			tampered.observation.i++;
		else if (field == 1)
			tampered.observation.q++;
		else if (field == 2)
			tampered.observation.phase++;
		else if (field == 3)
			tampered.observation.magnitude++;
		else if (field == 4)
			tampered.observation.epoch++;
		else
			tampered.observation.lineage++;
		CHECK(rejected_commit_is_unchanged(&first, &tampered),
		      "tampered observation field %u was accepted", field);
	}
	first_decision.ideal_phase = 0x8000;
	CHECK(rejected_commit_is_unchanged(&first, &first_decision),
	      "out-of-domain ideal phase was accepted");
	first_decision = make_decision(&first_observation, 1);
	first_decision.ideal_magnitude = 0x8000;
	CHECK(rejected_commit_is_unchanged(&first, &first_decision),
	      "out-of-domain ideal magnitude was accepted");
	first_decision = make_decision(&first_observation, 1);
	CHECK(v32_equalizer_commit(&first, &first_decision) ==
		      V32_EQUALIZER_OK,
	      "valid final lineage decision failed");
	saved = first;
	CHECK(v32_equalizer_commit(&first, &first_decision) ==
		      V32_EQUALIZER_INVALID_STATE &&
	      memcmp(&first, &saved, sizeof(saved)) == 0,
	      "double commit changed boundary state");
	return true;
}

static bool drive_carrier_error(
	struct v32_equalizer *equalizer, uint16_t phase_error)
{
	struct v32_equalizer_observation observation;
	struct v32_equalizer_decision decision;
	int16_t samples[3] = { 0 };
	size_t consumed;
	size_t need = equalizer->samples_until_observation;

	CHECK(v32_equalizer_process(
		      equalizer, samples, need, &consumed, &observation) ==
		      V32_EQUALIZER_OBSERVATION_READY && consumed == need,
	      "carrier boundary observation failed");
	decision = (struct v32_equalizer_decision) {
		.observation = observation,
		.ideal_phase = (uint16_t)(
			(observation.phase + 0x8000u - phase_error) & 0x7fffu),
		.ideal_magnitude = 0,
		.adapt_on_error = false,
	};
	CHECK(v32_equalizer_commit(equalizer, &decision) == V32_EQUALIZER_OK,
	      "carrier boundary decision failed");
	return true;
}

static bool test_carrier_loop_boundaries(void)
{
	struct v32_equalizer equalizer;
	bool observed_hysteresis = false;

	CHECK(v32_equalizer_init(&equalizer) == V32_EQUALIZER_OK &&
	      v32_equalizer_set_adaptation(
		      &equalizer, V32_EQUALIZER_ADAPTATION_DISABLED) ==
		      V32_EQUALIZER_OK,
	      "carrier boundary owner did not initialize");
	for (unsigned int count = 1; count <= 1502; ++count) {
		CHECK(drive_carrier_error(&equalizer, 0),
		      "carrier warmup drive failed at %u", count);
		if (count == 1499 || count == 1500)
			CHECK(equalizer.carrier_training_symbols == count &&
			      equalizer.carrier_gain_index == 0,
			      "carrier warmup boundary %u changed", count);
		if (count == 1501)
			CHECK(equalizer.carrier_training_symbols == 1501 &&
			      equalizer.carrier_gain_index == 1,
			      "carrier high-gain entry changed");
	}
	CHECK(equalizer.carrier_training_symbols == 1501 &&
	      equalizer.carrier_gain_index == 2,
	      "carrier low-error entry changed");
	for (unsigned int count = 0;
	     count < 64 && equalizer.carrier_gain_index != 1; ++count)
		CHECK(drive_carrier_error(&equalizer, 0x3000),
		      "carrier high-error drive failed");
	CHECK(equalizer.carrier_gain_index == 1,
	      "carrier high-error threshold was not reached");
	for (unsigned int count = 0;
	     count < 64 && equalizer.carrier_gain_index != 2; ++count) {
		int32_t absolute_error = equalizer.smoothed_carrier_error < 0 ?
			-(int32_t)equalizer.smoothed_carrier_error :
			equalizer.smoothed_carrier_error;

		if (absolute_error < 0x1988 && absolute_error > 0x0666) {
			CHECK(equalizer.carrier_gain_index == 1,
			      "carrier gain changed inside hysteresis band");
			observed_hysteresis = true;
		}
		CHECK(drive_carrier_error(&equalizer, 0),
		      "carrier low-error drive failed");
	}
	CHECK(observed_hysteresis && equalizer.carrier_gain_index == 2,
	      "carrier low threshold/hysteresis was not exercised");
	return true;
}

static bool test_lifecycle_and_adaptation_gates(void)
{
	struct v32_equalizer equalizer;
	struct v32_equalizer saved;
	struct v32_equalizer_observation observation;
	struct v32_equalizer_decision decision;
	int16_t samples[3] = { 21000, -17000, 9000 };
	int16_t coefficients[V32_EQUALIZER_TAPS];
	size_t consumed;

	CHECK(v32_equalizer_init(&equalizer) == V32_EQUALIZER_OK &&
	      v32_equalizer_set_adaptation(
		      &equalizer, V32_EQUALIZER_ADAPTATION_DISABLED) ==
		      V32_EQUALIZER_OK &&
	      v32_equalizer_process(
		      &equalizer, samples, 1, &consumed, &observation) ==
		      V32_EQUALIZER_OBSERVATION_READY,
	      "disabled adaptation fixture failed");
	memcpy(coefficients, equalizer.i_coefficients, sizeof(coefficients));
	decision = make_decision(&observation, 3);
	decision.adapt_on_error = true;
	CHECK(v32_equalizer_commit(&equalizer, &decision) == V32_EQUALIZER_OK &&
	      memcmp(coefficients, equalizer.i_coefficients,
		     sizeof(coefficients)) == 0 &&
	      equalizer.decision_error > 0 &&
	      equalizer.carrier_training_symbols == 1,
	      "disabled adaptation changed coefficients or skipped loops");
	CHECK(v32_equalizer_set_adaptation(
		      &equalizer, V32_EQUALIZER_ADAPTATION_COARSE) ==
		      V32_EQUALIZER_OK &&
	      v32_equalizer_process(
		      &equalizer, samples, 3, &consumed, &observation) ==
		      V32_EQUALIZER_OBSERVATION_READY,
	      "per-decision adaptation fixture failed");
	memcpy(coefficients, equalizer.i_coefficients, sizeof(coefficients));
	decision = make_decision(&observation, 4);
	decision.adapt_on_error = false;
	CHECK(v32_equalizer_commit(&equalizer, &decision) == V32_EQUALIZER_OK &&
	      memcmp(coefficients, equalizer.i_coefficients,
		     sizeof(coefficients)) == 0,
	      "false per-decision gate changed coefficients");
	CHECK(v32_equalizer_set_carrier_tracking(&equalizer, false) ==
		      V32_EQUALIZER_OK &&
	      v32_equalizer_process(
		      &equalizer, samples, 3, &consumed, &observation) ==
		      V32_EQUALIZER_OBSERVATION_READY,
	      "carrier freeze fixture failed");
	decision = (struct v32_equalizer_decision) {
		.observation = observation,
		.ideal_phase = (uint16_t)(observation.phase ^ 0x2000u),
		.ideal_magnitude = ideal_magnitude(5),
		.adapt_on_error = adapt_on_error(5),
	};
	saved = equalizer;
	CHECK(v32_equalizer_commit(&equalizer, &decision) == V32_EQUALIZER_OK &&
	      equalizer.smoothed_carrier_error !=
		      saved.smoothed_carrier_error &&
	      equalizer.carrier_integrator == saved.carrier_integrator &&
	      equalizer.carrier_phase_accumulator ==
		      saved.carrier_phase_accumulator &&
	      equalizer.carrier_training_symbols ==
		      saved.carrier_training_symbols,
	      "frozen carrier did not smooth error or changed loop state");
	CHECK(v32_equalizer_set_carrier_tracking(&equalizer, true) ==
		      V32_EQUALIZER_OK &&
	      v32_equalizer_process(
		      &equalizer, samples, 3, &consumed, &observation) ==
		      V32_EQUALIZER_OBSERVATION_READY,
	      "carrier resume fixture failed");
	decision = make_decision(&observation, 6);
	CHECK(v32_equalizer_commit(&equalizer, &decision) == V32_EQUALIZER_OK,
	      "carrier resume decision failed");
	saved = equalizer;
	CHECK(saved.carrier_training_symbols != 0 &&
	      v32_equalizer_restart_carrier_acquisition(&equalizer) ==
		      V32_EQUALIZER_OK &&
	      equalizer.carrier_training_symbols == 0 &&
	      memcmp(equalizer.i_coefficients, saved.i_coefficients,
		     sizeof(equalizer.i_coefficients)) == 0 &&
	      memcmp(equalizer.q_coefficients, saved.q_coefficients,
		     sizeof(equalizer.q_coefficients)) == 0 &&
	      memcmp(equalizer.sample_history, saved.sample_history,
		     sizeof(equalizer.sample_history)) == 0 &&
	      equalizer.history_index == saved.history_index &&
	      equalizer.samples_until_observation ==
		      saved.samples_until_observation &&
	      equalizer.carrier_phase_index == saved.carrier_phase_index &&
	      equalizer.carrier_phase_accumulator ==
		      saved.carrier_phase_accumulator &&
	      equalizer.carrier_integrator == saved.carrier_integrator,
	      "carrier counter restart disturbed continuous receive state");
	return true;
}

static bool test_rejected_calls_are_transactional(void)
{
	union {
		size_t consumed;
		struct v32_equalizer_observation observation;
	} overlapping;
	struct v32_equalizer equalizer;
	struct v32_equalizer saved;
	struct v32_equalizer_observation observation = {
		.i = 111, .q = -222, .phase = 333, .magnitude = 444,
		.epoch = 555, .lineage = 666,
	};
	struct v32_equalizer_observation saved_observation = observation;
	int16_t sample = 1234;
	size_t consumed = 987;

	CHECK(v32_equalizer_init(&equalizer) == V32_EQUALIZER_OK,
	      "transaction owner did not initialize");
	saved = equalizer;
	CHECK(v32_equalizer_process(
		      &equalizer, NULL, 1, &consumed, &observation) ==
		      V32_EQUALIZER_INVALID_ARGUMENT &&
	      memcmp(&equalizer, &saved, sizeof(saved)) == 0 &&
	      consumed == 987 &&
	      observations_equal(&observation, &saved_observation),
	      "null-input rejection changed state or outputs");
	CHECK(v32_equalizer_process(
		      &equalizer, &sample, 1, NULL, &observation) ==
		      V32_EQUALIZER_INVALID_ARGUMENT &&
	      v32_equalizer_process(
		      &equalizer, &sample, 1, &consumed, NULL) ==
		      V32_EQUALIZER_INVALID_ARGUMENT &&
	      memcmp(&equalizer, &saved, sizeof(saved)) == 0,
	      "null output rejection changed owner state");
	CHECK(v32_equalizer_process(
		      &equalizer, (const int16_t *)&equalizer, 1,
		      &consumed, &observation) ==
		      V32_EQUALIZER_INVALID_ARGUMENT &&
	      v32_equalizer_process(
		      &equalizer, &sample, 1,
		      (size_t *)((unsigned char *)&equalizer + 1),
		      &observation) == V32_EQUALIZER_INVALID_ARGUMENT &&
	      v32_equalizer_process(
		      &equalizer, &sample, 1,
		      &overlapping.consumed, &overlapping.observation) ==
		      V32_EQUALIZER_INVALID_ARGUMENT &&
	      memcmp(&equalizer, &saved, sizeof(saved)) == 0,
	      "overlap rejection changed owner state");
	const int invalid_modes[] = {
		-1, 3, 0x10000, 0x10001, 0x10002, -0x10000, INT_MIN, INT_MAX,
	};
	for (size_t index = 0; index < ARRAY_LENGTH(invalid_modes); ++index) {
		CHECK(v32_equalizer_set_adaptation(
			      &equalizer, (enum v32_equalizer_adaptation)invalid_modes[index]) ==
			      V32_EQUALIZER_INVALID_ARGUMENT &&
		      memcmp(&equalizer, &saved, sizeof(saved)) == 0,
		      "adaptation %d changed owner state", invalid_modes[index]);
	}
	equalizer.history_index = V32_EQUALIZER_TAPS;
	CHECK(!v32_equalizer_is_valid(&equalizer) &&
	      v32_equalizer_process(
		      &equalizer, &sample, 1,
		      &consumed, &observation) == V32_EQUALIZER_INVALID_STATE,
	      "invalid inactive cursor was accepted");
	equalizer = saved;
	equalizer.i_coefficients[0] = INT16_MIN;
	equalizer.sample_history[0] = INT16_MIN;
	CHECK(v32_equalizer_is_valid(&equalizer),
	      "valid int16 learned/sample states were overconstrained");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "observation comparison", test_observation_comparison },
		{ "initial profile", test_initial_profile },
		{ "exact oracle", test_exact_oracle },
		{ "fragment equivalence", test_fragment_equivalence },
		{ "lineage and pending", test_lineage_and_pending_contract },
		{ "lifecycle and adaptation", test_lifecycle_and_adaptation_gates },
		{ "carrier loop boundaries", test_carrier_loop_boundaries },
		{ "transactional rejection", test_rejected_calls_are_transactional },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		bool passed;

		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		passed = tests[index].run();
		if (!passed) {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failures++;
		} else {
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		}
	}
	return failures == 0 ? 0 : 1;
}
