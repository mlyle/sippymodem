// SPDX-License-Identifier: BSD-3-Clause

#include <v34_adaptive_equalizer.h>
#include <v34_data_quality.h>

#include "test.h"

#include <inttypes.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
	ORACLE_SYMBOLS = 96,
	FEEDBACK_START_SYMBOL = 12,
};

static bool test_training_decoder_power(void)
{
	static const struct {
		uint16_t rate;
		uint16_t middle;
	} schedules[] = {
		{ 2400, 1200 }, { 2743, 1371 }, { 2800, 1400 },
		{ 3000, 1500 }, { 3200, 1600 }, { 3429, 1714 },
	};

	for (size_t index = 0; index < sizeof(schedules) / sizeof(*schedules);
	     ++index) {
		uint16_t rate = schedules[index].rate;
		uint16_t middle = schedules[index].middle;

		CHECK(v34_training_decoder_power(rate, 0u) == 0x7000 &&
		      v34_training_decoder_power(rate, middle - 1u) == 0x7000 &&
		      v34_training_decoder_power(rate, middle) == 0x4000 &&
		      v34_training_decoder_power(rate, rate - 1u) == 0x4000 &&
		      v34_training_decoder_power(rate, rate) == 0x2000 &&
		      v34_training_decoder_power(rate, rate + 1u) == 0x2000 &&
		      v34_training_decoder_power(rate, UINT32_MAX) == 0x2000,
		      "training power boundaries changed at %u symbols/s", rate);
	}
	return true;
}

static uint64_t hash_word(uint64_t hash, uint16_t word)
{
	return (hash ^ word) * UINT64_C(1099511628211);
}

static uint64_t hash_u32(uint64_t hash, uint32_t value)
{
	hash = hash_word(hash, (uint16_t)value);
	return hash_word(hash, (uint16_t)(value >> 16));
}

static uint64_t hash_s16_array(
	uint64_t hash, const int16_t *values, size_t count)
{
	for (size_t index = 0; index < count; ++index)
		hash = hash_word(hash, (uint16_t)values[index]);
	return hash;
}

static uint64_t hash_u16_array(
	uint64_t hash, const uint16_t *values, size_t count)
{
	for (size_t index = 0; index < count; ++index)
		hash = hash_word(hash, values[index]);
	return hash;
}

static uint64_t state_hash(
	const struct v34_adaptive_equalizer *equalizer)
{
	uint64_t hash = UINT64_C(14695981039346656037);

	hash = hash_s16_array(
		hash, equalizer->equalizer.in_phase_delay,
		V34_EQUALIZER_TAPS);
	hash = hash_s16_array(
		hash, equalizer->equalizer.quadrature_delay,
		V34_EQUALIZER_TAPS);
	hash = hash_s16_array(
		hash, equalizer->equalizer.in_phase_coefficient,
		V34_EQUALIZER_TAPS);
	hash = hash_s16_array(
		hash, equalizer->equalizer.quadrature_coefficient,
		V34_EQUALIZER_TAPS);
	hash = hash_u16_array(
		hash, equalizer->equalizer.in_phase_residue,
		V34_EQUALIZER_TAPS);
	hash = hash_u16_array(
		hash, equalizer->equalizer.quadrature_residue,
		V34_EQUALIZER_TAPS);
	hash = hash_s16_array(
		hash, equalizer->feedback.coefficient_a, 3);
	hash = hash_s16_array(
		hash, equalizer->feedback.coefficient_b, 3);
	hash = hash_s16_array(
		hash, equalizer->feedback.in_phase_history, 4);
	hash = hash_s16_array(
		hash, equalizer->feedback.quadrature_history, 4);
	hash = hash_word(hash, (uint16_t)equalizer->equalizer.index);
	hash = hash_u32(hash, (uint32_t)equalizer->phase_accumulator);
	hash = hash_u32(hash, (uint32_t)equalizer->phase_error);
	hash = hash_word(hash, (uint16_t)equalizer->rotate_in_phase);
	hash = hash_word(hash, (uint16_t)equalizer->rotate_quadrature);
	hash = hash_word(hash, equalizer->carrier_phase_low);
	hash = hash_word(hash, equalizer->carrier_phase_fraction);
	hash = hash_u32(hash, (uint32_t)equalizer->epoch);
	return hash_u32(hash, (uint32_t)(equalizer->epoch >> 32));
}

static int16_t next_sample(uint32_t *generator)
{
	*generator = *generator * UINT32_C(1664525) +
		UINT32_C(1013904223);
	return (int16_t)(uint16_t)(*generator >> 8);
}

static struct v34_adaptive_equalizer_decision zero_decision(
	const struct v34_adaptive_equalizer_observation *observation)
{
	return (struct v34_adaptive_equalizer_decision){
		.observation = *observation,
		.decoder_power_q15 = 0x4000,
		.adaptation = V34_ADAPTIVE_EQUALIZER_CENTER,
	};
}

static bool push_zero_symbol(struct v34_adaptive_equalizer *equalizer)
{
	struct v34_analytic_sample zero = { 0 };

	return v34_adaptive_equalizer_push(equalizer, zero) ==
		       V34_ADAPTIVE_EQUALIZER_OK &&
	       v34_adaptive_equalizer_push(equalizer, zero) ==
		       V34_ADAPTIVE_EQUALIZER_OK;
}

static bool test_initial_state_and_carrier_profile(void)
{
	uint64_t profile_hash = UINT64_C(14695981039346656037);

	for (uint16_t coarse = 0; coarse < 0x2000u; ++coarse) {
		struct v34_adaptive_equalizer equalizer;
		struct v34_adaptive_equalizer_observation observation;
		struct v34_adaptive_equalizer_decision decision;

		CHECK(v34_adaptive_equalizer_init(&equalizer) ==
			      V34_ADAPTIVE_EQUALIZER_OK &&
		      v34_adaptive_equalizer_is_valid(&equalizer),
		      "adaptive equalizer did not initialize at %u", coarse);
		CHECK(equalizer.equalizer.in_phase_coefficient[
			      V34_EQUALIZER_IDENTITY_TAP] == 0x1000 &&
		      equalizer.rotate_in_phase == 0x4000 &&
		      equalizer.rotate_quadrature == 0 &&
		      equalizer.epoch == 0 &&
		      equalizer.feedback_mode ==
			      V34_ADAPTIVE_EQUALIZER_FEEDBACK_NONE &&
		      !equalizer.decision_pending,
		      "adaptive equalizer initial state changed");
		equalizer.carrier_phase_fraction = coarse;
		CHECK(push_zero_symbol(&equalizer),
		      "carrier profile input failed at %u", coarse);
		CHECK(v34_adaptive_equalizer_observe(
			      &equalizer, &observation) ==
			      V34_ADAPTIVE_EQUALIZER_OK,
		      "carrier profile observation failed at %u", coarse);
		decision = zero_decision(&observation);
		CHECK(v34_adaptive_equalizer_commit(&equalizer, &decision) ==
			      V34_ADAPTIVE_EQUALIZER_OK,
		      "carrier profile commit failed at %u", coarse);
		profile_hash = hash_word(
			hash_word(profile_hash,
				  (uint16_t)equalizer.rotate_in_phase),
			(uint16_t)equalizer.rotate_quadrature);
	}
	CHECK(profile_hash == UINT64_C(0x9fdebca0c6f129f5),
	      "carrier profile changed: %#llx",
	      (unsigned long long)profile_hash);

	return true;
}

static bool test_carrier_phase_accumulation(void)
{
	static const uint32_t initial_phases[] = {
		0, UINT32_C(0x17fff), UINT32_C(0x18000),
		UINT32_C(0x1ffff), UINT32_C(0x1fffffff),
	};
	static const int32_t steps[] = { -65537, -32768, -1, 0, 1, 32768, 65537 };
	const uint32_t phase_mask = UINT32_C(0x1fffffff);

	for (size_t initial = 0; initial < ARRAY_LENGTH(initial_phases); ++initial) {
		for (size_t step = 0; step < ARRAY_LENGTH(steps); ++step) {
			struct v34_adaptive_equalizer equalizer;
			uint32_t expected = initial_phases[initial];

			CHECK(v34_adaptive_equalizer_init(&equalizer) ==
				      V34_ADAPTIVE_EQUALIZER_OK,
			      "carrier accumulation owner did not initialize");
			equalizer.carrier_phase_fraction = (uint16_t)(expected >> 16);
			equalizer.carrier_phase_low = (uint16_t)expected;
			equalizer.phase_accumulator = steps[step] * 32;
			for (size_t epoch = 0; epoch < 1024; ++epoch) {
				struct v34_adaptive_equalizer_observation observation;
				struct v34_adaptive_equalizer_decision decision;
				uint32_t actual;

				CHECK(push_zero_symbol(&equalizer) &&
				      v34_adaptive_equalizer_observe(
					      &equalizer, &observation) ==
					      V34_ADAPTIVE_EQUALIZER_OK,
				      "carrier accumulation observation failed");
				decision = zero_decision(&observation);
				decision.adaptation = V34_ADAPTIVE_EQUALIZER_CARRIER;
				CHECK(v34_adaptive_equalizer_commit(
					      &equalizer, &decision) ==
					      V34_ADAPTIVE_EQUALIZER_OK,
				      "carrier accumulation commit failed");
				expected = (expected + (uint32_t)steps[step]) & phase_mask;
				actual = ((uint32_t)equalizer.carrier_phase_fraction << 16) |
					 equalizer.carrier_phase_low;
				CHECK(actual == expected && equalizer.phase_error == 0 &&
				      equalizer.phase_accumulator == steps[step] * 32,
				      "carrier phase drifted: initial=%#x step=%d epoch=%zu "
				      "expected=%#x actual=%#x",
				      initial_phases[initial], steps[step], epoch,
				      expected, actual);
			}
		}
	}
	return true;
}

static bool test_carrier_coast(void)
{
	const uint32_t phase_mask = UINT32_C(0x1fffffff);
	static const int32_t steps[] = { -65537, -1, 0, 1, 65537 };

	for (unsigned tracking = 0; tracking < 2; ++tracking) {
		for (size_t step = 0; step < ARRAY_LENGTH(steps); ++step) {
			struct v34_adaptive_equalizer equalizer;
			uint32_t expected = phase_mask;

			CHECK(v34_adaptive_equalizer_init(&equalizer) ==
				      V34_ADAPTIVE_EQUALIZER_OK &&
			      v34_adaptive_equalizer_set_data_carrier_tracking(
				      &equalizer, tracking != 0) ==
				      V34_ADAPTIVE_EQUALIZER_OK,
			      "carrier coast initialization failed");
			equalizer.carrier_phase_fraction = (uint16_t)(expected >> 16);
			equalizer.carrier_phase_low = (uint16_t)expected;
			equalizer.phase_accumulator = steps[step] * 32;
			equalizer.phase_error = 12345;
			for (unsigned epoch = 0; epoch < 16; ++epoch) {
				struct v34_adaptive_equalizer_observation observation, changed;
				struct v34_adaptive_equalizer saved, discarded;

				CHECK(push_zero_symbol(&equalizer) &&
				      v34_adaptive_equalizer_observe(&equalizer, &observation) ==
					      V34_ADAPTIVE_EQUALIZER_OK,
				      "carrier coast observation failed");
				saved = equalizer;
				changed = observation;
				changed.epoch++;
				CHECK(v34_adaptive_equalizer_coast(&equalizer, &changed) ==
					      V34_ADAPTIVE_EQUALIZER_INVALID_ARGUMENT &&
				      v34_adaptive_equalizer_coast(&equalizer, NULL) ==
					      V34_ADAPTIVE_EQUALIZER_INVALID_ARGUMENT &&
				      memcmp(&equalizer, &saved, sizeof(equalizer)) == 0,
				      "rejected carrier coast changed the owner");
				discarded = equalizer;
				CHECK(v34_adaptive_equalizer_discard(&discarded, &observation) ==
					      V34_ADAPTIVE_EQUALIZER_OK &&
				      v34_adaptive_equalizer_coast(&equalizer, &observation) ==
					      V34_ADAPTIVE_EQUALIZER_OK,
				      "carrier coast failed");
				expected = (expected + (uint32_t)steps[step]) & phase_mask;
				CHECK((((uint32_t)equalizer.carrier_phase_fraction << 16) |
				       equalizer.carrier_phase_low) == expected &&
				      v34_adaptive_equalizer_is_valid(&equalizer),
				      "carrier coast lost frequency or phase wrap");
				/* Only carrier phase may differ from a targetless discard:
				 * frequency, FIR, feedback and quality must not learn. */
				discarded.carrier_phase_low = equalizer.carrier_phase_low;
				discarded.carrier_phase_fraction = equalizer.carrier_phase_fraction;
				discarded.rotate_in_phase = equalizer.rotate_in_phase;
				discarded.rotate_quadrature = equalizer.rotate_quadrature;
				CHECK(memcmp(&equalizer, &discarded, sizeof(equalizer)) == 0,
				      "carrier coast learned from an erased observation");
			}
		}
	}
	return true;
}

static bool test_carrier_only_commit(void)
{
	const struct v34_analytic_sample input = { 3000, -1000 };
	struct v34_adaptive_equalizer equalizer;
	struct v34_adaptive_equalizer saved;
	struct v34_adaptive_equalizer_observation observation;

	CHECK(v34_adaptive_equalizer_init(&equalizer) ==
		      V34_ADAPTIVE_EQUALIZER_OK,
	      "carrier-only owner did not initialize");
	for (size_t epoch = 0;
	     epoch <= V34_ADAPTIVE_EQUALIZER_FIRST_SIGNAL_EPOCH; ++epoch) {
		struct v34_adaptive_equalizer_decision decision;

		CHECK(v34_adaptive_equalizer_push(&equalizer, input) ==
			      V34_ADAPTIVE_EQUALIZER_OK &&
		      v34_adaptive_equalizer_push(&equalizer, input) ==
			      V34_ADAPTIVE_EQUALIZER_OK &&
		      v34_adaptive_equalizer_observe(
			      &equalizer, &observation) ==
			      V34_ADAPTIVE_EQUALIZER_OK,
		      "carrier-only observation failed at %zu", epoch);
		decision = (struct v34_adaptive_equalizer_decision){
			.observation = observation,
			.target = observation.decision,
			.adaptation = V34_ADAPTIVE_EQUALIZER_SKIP,
		};
		if (epoch == V34_ADAPTIVE_EQUALIZER_FIRST_SIGNAL_EPOCH) {
			decision.target = (struct v34_analytic_sample){
				.in_phase = (int16_t)-observation.decision.quadrature,
				.quadrature = observation.decision.in_phase,
			};
			decision.decoder_power_q15 = 0x4000;
			decision.adaptation = V34_ADAPTIVE_EQUALIZER_CARRIER;
			saved = equalizer;
		}
		CHECK(v34_adaptive_equalizer_commit(&equalizer, &decision) ==
			      V34_ADAPTIVE_EQUALIZER_OK,
		      "carrier-only commit failed at %zu", epoch);
	}
	CHECK(memcmp(&equalizer.equalizer, &saved.equalizer,
		     sizeof(equalizer.equalizer)) == 0 &&
	      equalizer.phase_accumulator != saved.phase_accumulator &&
	      !equalizer.decision_pending &&
	      v34_adaptive_equalizer_is_valid(&equalizer),
	      "carrier-only commit changed the FIR or missed carrier tracking");
	return true;
}

static bool test_retained_pair_geometry(uint16_t taps, uint16_t delay)
{
	struct v34_adaptive_equalizer equalizer;
	struct v34_adaptive_equalizer first;
	struct v34_adaptive_equalizer expected;
	struct v34_adaptive_equalizer saved;
	struct v34_adaptive_equalizer_observation observation;
	struct v34_adaptive_equalizer_decision decisions[2];
	uint32_t generator = UINT32_C(0x42ac9581);

	CHECK(v34_adaptive_equalizer_init(&equalizer) ==
		      V34_ADAPTIVE_EQUALIZER_OK &&
	      v34_equalizer_init_geometry(
		      &equalizer.equalizer, taps, delay) == V34_EQUALIZER_OK,
	      "retained-pair geometry did not initialize");
	for (size_t epoch = 0; epoch < V34_EQUALIZER_MAX_TAPS + 2u; ++epoch) {
		for (size_t input = 0; input < 2u; ++input) {
			struct v34_analytic_sample point = {
				.in_phase = (int16_t)(next_sample(&generator) / 4),
				.quadrature = (int16_t)(next_sample(&generator) / 4),
			};

			CHECK(v34_adaptive_equalizer_push(&equalizer, point) ==
				      V34_ADAPTIVE_EQUALIZER_OK,
			      "retained-pair input failed");
		}
		CHECK(v34_adaptive_equalizer_observe(
			      &equalizer, &observation) == V34_ADAPTIVE_EQUALIZER_OK,
		      "retained-pair observation failed");
		if (epoch < V34_EQUALIZER_MAX_TAPS) {
			CHECK(v34_adaptive_equalizer_discard(
				      &equalizer, &observation) ==
				      V34_ADAPTIVE_EQUALIZER_OK,
			      "retained-pair prefix failed");
			continue;
		}
		decisions[epoch - V34_EQUALIZER_MAX_TAPS] =
			(struct v34_adaptive_equalizer_decision){
				.observation = observation,
				.target = { 2289, -2290 },
				.decoder_power_q15 = 0x1000,
				.adaptation = V34_ADAPTIVE_EQUALIZER_FULL,
			};
		if (epoch == V34_EQUALIZER_MAX_TAPS) {
			first = equalizer;
			CHECK(v34_adaptive_equalizer_retain_observation(
				      &equalizer, &observation) ==
				      V34_ADAPTIVE_EQUALIZER_OK &&
			      v34_adaptive_equalizer_is_valid(&equalizer) &&
			      equalizer.retained.pending &&
			      !equalizer.decision_pending &&
			      memcmp(&equalizer.equalizer, &first.equalizer,
				     sizeof(first.equalizer)) == 0,
			      "retaining a decision changed its FIR history");
			saved = equalizer;
			CHECK(v34_adaptive_equalizer_retain_observation(
				      &equalizer, &observation) ==
				      V34_ADAPTIVE_EQUALIZER_INVALID_STATE &&
			      v34_adaptive_equalizer_begin_tracking(&equalizer) ==
				      V34_ADAPTIVE_EQUALIZER_INVALID_STATE &&
			      memcmp(&equalizer, &saved, sizeof(saved)) == 0,
			      "a retained decision allowed a conflicting transition");
		}
	}
	decisions[1].target = (struct v34_analytic_sample){ -2290, -2290 };
	expected = equalizer;
	memset(&expected.retained, 0, sizeof(expected.retained));
	CHECK(v34_adaptive_equalizer_commit(&first, &decisions[0]) ==
		      V34_ADAPTIVE_EQUALIZER_OK,
	      "first independent gradient failed");
	/* Apply that historical gradient without replacing the second input
	 * window or its captured carrier frame. The second target was decided
	 * before the first target's carrier correction was committed. */
	memcpy(expected.equalizer.in_phase_coefficient,
	       first.equalizer.in_phase_coefficient,
	       sizeof(expected.equalizer.in_phase_coefficient));
	memcpy(expected.equalizer.quadrature_coefficient,
	       first.equalizer.quadrature_coefficient,
	       sizeof(expected.equalizer.quadrature_coefficient));
	memcpy(expected.equalizer.in_phase_residue,
	       first.equalizer.in_phase_residue,
	       sizeof(expected.equalizer.in_phase_residue));
	memcpy(expected.equalizer.quadrature_residue,
	       first.equalizer.quadrature_residue,
	       sizeof(expected.equalizer.quadrature_residue));
	expected.phase_accumulator = first.phase_accumulator;
	expected.carrier_phase_low = first.carrier_phase_low;
	expected.carrier_phase_fraction = first.carrier_phase_fraction;
	expected.rotate_in_phase = first.rotate_in_phase;
	expected.rotate_quadrature = first.rotate_quadrature;
	expected.quality = first.quality;
	CHECK(v34_adaptive_equalizer_commit(&expected, &decisions[1]) ==
		      V34_ADAPTIVE_EQUALIZER_OK,
	      "second independent gradient failed");
	saved = equalizer;
	decisions[0].observation.epoch++;
	CHECK(v34_adaptive_equalizer_commit_pair(&equalizer, decisions) ==
		      V34_ADAPTIVE_EQUALIZER_INVALID_ARGUMENT &&
	      memcmp(&equalizer, &saved, sizeof(saved)) == 0,
	      "a stale paired decision changed the receiver");
	decisions[0].observation.epoch--;
	CHECK(v34_adaptive_equalizer_commit_pair(&equalizer, decisions) ==
		      V34_ADAPTIVE_EQUALIZER_OK &&
	      !equalizer.retained.pending && !equalizer.decision_pending &&
	      memcmp(&equalizer.equalizer, &expected.equalizer,
		     sizeof(expected.equalizer)) == 0 &&
	      memcmp(&equalizer.quality, &expected.quality,
		     sizeof(expected.quality)) == 0 &&
	      equalizer.phase_accumulator == expected.phase_accumulator &&
	      equalizer.carrier_phase_low == expected.carrier_phase_low &&
	      equalizer.carrier_phase_fraction == expected.carrier_phase_fraction &&
	      equalizer.rotate_in_phase == expected.rotate_in_phase &&
	      equalizer.rotate_quadrature == expected.rotate_quadrature,
	      "paired gradients lost their FIR window or carrier frame");
	return true;
}

static bool test_data_carrier_tracking(void)
{
	const struct v34_analytic_sample input = { 12000, -4000 };
	const int16_t target_quadrature[] = { 0, -16000, -32000, 16000, 32000 };
	const uint32_t phase_mask = UINT32_C(0x1fffffff);
	const int32_t bounded_step =
		V34_TRAINING_FIXED_AMPLITUDE * V34_TRAINING_FIXED_AMPLITUDE / 20;
	struct v34_adaptive_equalizer equalizer;
	struct v34_adaptive_equalizer saved;
	struct v34_adaptive_equalizer_observation observation;
	int32_t learned_frequency;
	uint32_t uncorrected_phase = 0;

	CHECK(v34_adaptive_equalizer_init(&equalizer) ==
		      V34_ADAPTIVE_EQUALIZER_OK,
	      "data-carrier fixture did not initialize");
	for (size_t epoch = 0;
	     epoch <= V34_ADAPTIVE_EQUALIZER_FIRST_SIGNAL_EPOCH; ++epoch) {
		CHECK(v34_adaptive_equalizer_push(&equalizer, input) ==
			      V34_ADAPTIVE_EQUALIZER_OK &&
		      v34_adaptive_equalizer_push(&equalizer, input) ==
			      V34_ADAPTIVE_EQUALIZER_OK &&
		      v34_adaptive_equalizer_observe(&equalizer, &observation) ==
			      V34_ADAPTIVE_EQUALIZER_OK,
		      "data-carrier acquisition failed at epoch %zu", epoch);
		if (epoch != V34_ADAPTIVE_EQUALIZER_FIRST_SIGNAL_EPOCH)
			CHECK(v34_adaptive_equalizer_discard(
				      &equalizer, &observation) ==
				      V34_ADAPTIVE_EQUALIZER_OK,
			      "data-carrier acquisition prefix did not advance");
	}
	{
		const struct v34_adaptive_equalizer_decision training = {
			.observation = observation,
			.target = { observation.decision.in_phase,
				    (int16_t)(observation.decision.quadrature + 1) },
			.decoder_power_q15 = 0x1000,
			.adaptation = V34_ADAPTIVE_EQUALIZER_CARRIER,
		};

		CHECK(v34_adaptive_equalizer_commit(&equalizer, &training) ==
			      V34_ADAPTIVE_EQUALIZER_OK,
		      "data-carrier fixture did not learn frequency");
	}
	learned_frequency = equalizer.phase_accumulator;
	CHECK(learned_frequency != 0,
	      "carrier fixture learned no frequency from %d/%d",
	      observation.decision.in_phase, observation.decision.quadrature);
	CHECK(v34_adaptive_equalizer_set_data_carrier_tracking(
		      &equalizer, true) == V34_ADAPTIVE_EQUALIZER_OK &&
	      equalizer.phase_accumulator == learned_frequency,
	      "data-carrier transition lost its learned frequency");
	CHECK(v34_adaptive_equalizer_push(&equalizer, input) ==
		      V34_ADAPTIVE_EQUALIZER_OK &&
	      v34_adaptive_equalizer_push(&equalizer, input) ==
		      V34_ADAPTIVE_EQUALIZER_OK &&
	      v34_adaptive_equalizer_observe(&equalizer, &observation) ==
		      V34_ADAPTIVE_EQUALIZER_OK &&
	      observation.decision.in_phase > 1000,
	      "data-carrier fixture did not produce an in-phase decision: %d/%d",
	      observation.decision.in_phase, observation.decision.quadrature);
	saved = equalizer;
	CHECK(v34_adaptive_equalizer_set_data_carrier_tracking(
		      &equalizer, false) == V34_ADAPTIVE_EQUALIZER_INVALID_STATE &&
	      memcmp(&equalizer, &saved, sizeof(equalizer)) == 0,
	      "pending observation allowed a carrier-policy transition");
	for (size_t index = 0; index < ARRAY_LENGTH(target_quadrature); ++index) {
		struct v34_adaptive_equalizer corrected = equalizer;
		const struct v34_adaptive_equalizer_decision decision = {
			.observation = observation,
			.target = { 0, target_quadrature[index] },
			.decoder_power_q15 = 0x1000,
			.adaptation = V34_ADAPTIVE_EQUALIZER_CARRIER,
		};
		uint32_t phase;
		uint32_t expected_delta = (uint32_t)(index == 0 ? 0 :
			(target_quadrature[index] < 0 ? bounded_step : -bounded_step));

		CHECK(v34_adaptive_equalizer_commit(&corrected, &decision) ==
			      V34_ADAPTIVE_EQUALIZER_OK &&
		      corrected.phase_accumulator == learned_frequency &&
		      memcmp(&corrected.equalizer, &equalizer.equalizer,
			     sizeof(equalizer.equalizer)) == 0,
		      "data-carrier feedback changed learned frequency or FIR");
		phase = ((uint32_t)corrected.carrier_phase_fraction << 16u) |
			corrected.carrier_phase_low;
		if (index == 0)
			uncorrected_phase = phase;
		CHECK(((phase - uncorrected_phase) & phase_mask) ==
			      (expected_delta & phase_mask),
		      "data-carrier phase feedback exceeded its bound at %zu", index);
	}
	CHECK(v34_adaptive_equalizer_retain_observation(
		      &equalizer, &observation) == V34_ADAPTIVE_EQUALIZER_OK,
	      "data-carrier observation could not be retained");
	saved = equalizer;
	CHECK(v34_adaptive_equalizer_set_data_carrier_tracking(
		      &equalizer, false) == V34_ADAPTIVE_EQUALIZER_INVALID_STATE &&
	      memcmp(&equalizer, &saved, sizeof(equalizer)) == 0,
	      "retained observation allowed a carrier-policy transition");
	return true;
}

static bool test_exact_decision_feedback_oracle(void)
{
	static const struct v34_analytic_sample targets[] = {
		{ 2289, 2289 },
		{ 2289, -2290 },
		{ -2290, -2290 },
		{ -2290, 2289 },
	};
	struct v34_adaptive_equalizer equalizer;
	uint32_t generator = UINT32_C(0x4d595df4);
	uint64_t observation_hash = UINT64_C(14695981039346656037);

	CHECK(v34_adaptive_equalizer_init(&equalizer) ==
		      V34_ADAPTIVE_EQUALIZER_OK,
	      "oracle equalizer did not initialize");
	for (size_t symbol = 0; symbol < ORACLE_SYMBOLS; ++symbol) {
		struct v34_adaptive_equalizer_observation observation;
		struct v34_adaptive_equalizer_decision decision;

		if (symbol == FEEDBACK_START_SYMBOL)
			CHECK(v34_adaptive_equalizer_set_feedback_mode(
				      &equalizer,
				      V34_ADAPTIVE_EQUALIZER_FEEDBACK_LEARN |
					      V34_ADAPTIVE_EQUALIZER_FEEDBACK_APPLY) ==
				      V34_ADAPTIVE_EQUALIZER_OK,
			      "oracle feedback transition failed");
		for (size_t input = 0; input < 2; ++input) {
			struct v34_analytic_sample sample = {
				.in_phase = next_sample(&generator),
				.quadrature = next_sample(&generator),
			};

			CHECK(v34_adaptive_equalizer_push(
				      &equalizer, sample) ==
				      V34_ADAPTIVE_EQUALIZER_OK,
			      "oracle input failed at symbol %zu", symbol);
		}
		CHECK(v34_adaptive_equalizer_observe(
			      &equalizer, &observation) ==
			      V34_ADAPTIVE_EQUALIZER_OK,
		      "oracle observation failed at symbol %zu", symbol);
		observation_hash = hash_word(
			hash_word(observation_hash,
				  (uint16_t)observation.decision.in_phase),
			(uint16_t)observation.decision.quadrature);
		decision = (struct v34_adaptive_equalizer_decision){
			.observation = observation,
			.target = targets[symbol % ARRAY_LENGTH(targets)],
		};
		if (symbol < 24) {
			decision.decoder_power_q15 = 0x4000;
			decision.adaptation = V34_ADAPTIVE_EQUALIZER_CENTER;
		} else if (symbol >= 72 && (symbol & 1u) == 0) {
			decision.adaptation = V34_ADAPTIVE_EQUALIZER_SKIP;
		} else {
			decision.decoder_power_q15 = 0x2000;
			decision.adaptation = V34_ADAPTIVE_EQUALIZER_FULL;
		}
		CHECK(v34_adaptive_equalizer_commit(&equalizer, &decision) ==
			      V34_ADAPTIVE_EQUALIZER_OK,
		      "oracle commit failed at symbol %zu", symbol);
	}
	CHECK(observation_hash == UINT64_C(0x64e46e6432bf9a5d),
	      "observation oracle changed: %#llx",
	      (unsigned long long)observation_hash);
	CHECK(state_hash(&equalizer) == UINT64_C(0xbdaa3e5601caa320),
	      "terminal oracle changed: %#llx",
	      (unsigned long long)state_hash(&equalizer));
	CHECK(equalizer.phase_accumulator == -1671967 &&
	      equalizer.phase_error == 154685984 &&
	      equalizer.rotate_in_phase == 3833 &&
	      equalizer.rotate_quadrature == -15930 &&
	      equalizer.carrier_phase_low == 30664 &&
	      equalizer.carrier_phase_fraction == 6452 &&
	      equalizer.equalizer.index == 32 && equalizer.epoch == 96 &&
	      equalizer.lineage == UINT64_C(0xd3e3fb96b7dabaf6) &&
	      !equalizer.decision_pending &&
	      v34_adaptive_equalizer_is_valid(&equalizer),
	      "terminal oracle scalar state changed");
	return true;
}

static bool test_feedback_history_lineage(void)
{
	struct v34_adaptive_equalizer equalizer;
	struct v34_adaptive_equalizer_observation observation;
	struct v34_adaptive_equalizer_decision decision;
	static const int16_t in_phase_history[] = { 20, 30, 40, 50 };
	static const int16_t quadrature_history[] = { 5, 7, 11, 13 };

	CHECK(v34_adaptive_equalizer_init(&equalizer) ==
		      V34_ADAPTIVE_EQUALIZER_OK &&
	      v34_adaptive_equalizer_set_feedback_mode(
		      &equalizer, V34_ADAPTIVE_EQUALIZER_FEEDBACK_LEARN) ==
		      V34_ADAPTIVE_EQUALIZER_OK,
	      "feedback-history owner did not initialize");
	equalizer.feedback.coefficient_a[0] = 0x2000;
	memcpy(equalizer.feedback.in_phase_history, in_phase_history,
	       sizeof(in_phase_history));
	memcpy(equalizer.feedback.quadrature_history, quadrature_history,
	       sizeof(quadrature_history));
	CHECK(push_zero_symbol(&equalizer) &&
	      v34_adaptive_equalizer_observe(&equalizer, &observation) ==
		      V34_ADAPTIVE_EQUALIZER_OK,
	      "feedback-history observation failed");
	decision = (struct v34_adaptive_equalizer_decision){
		.observation = observation,
		.target = { -100, 50 },
		.adaptation = V34_ADAPTIVE_EQUALIZER_SKIP,
	};
	CHECK(v34_adaptive_equalizer_commit(&equalizer, &decision) ==
		      V34_ADAPTIVE_EQUALIZER_OK &&
	      memcmp(equalizer.feedback.in_phase_history,
		     (const int16_t[]){ 100, 20, 30, 40 },
		     sizeof(equalizer.feedback.in_phase_history)) == 0 &&
	      memcmp(equalizer.feedback.quadrature_history,
		     (const int16_t[]){ -50, 5, 7, 11 },
		     sizeof(equalizer.feedback.quadrature_history)) == 0,
	      "feedback history retained the corrected residual");
	return true;
}

static int16_t feedback_oracle_sample(int64_t sum)
{
	uint32_t word = (uint32_t)sum;
	int64_t signed_word = word <= INT32_MAX ? word :
		(int64_t)word - INT64_C(4294967296);
	int64_t shifted = signed_word >= 0 ? signed_word / 16384 :
		-((-signed_word + 16383) / 16384);
	uint16_t sample = (uint16_t)shifted;

	return sample <= INT16_MAX ? (int16_t)sample :
		(int16_t)((int32_t)sample - 65536);
}

static bool test_feedback_full_scale_projection(void)
{
	const int16_t edges[] = { INT16_MIN, INT16_MAX, -1, 0, 1 };
	uint32_t random = UINT32_C(0x671a9c31);

	for (unsigned trial = 0; trial < 4096; ++trial) {
		struct v34_adaptive_equalizer equalizer;
		struct v34_adaptive_equalizer_observation observation;
		struct v34_adaptive_equalizer_feedback saved;
		int64_t real = -8192, imaginary = 8192;

		CHECK(v34_adaptive_equalizer_init(&equalizer) ==
			      V34_ADAPTIVE_EQUALIZER_OK &&
		      v34_adaptive_equalizer_set_feedback_mode(&equalizer,
			      V34_ADAPTIVE_EQUALIZER_FEEDBACK_APPLY) ==
			      V34_ADAPTIVE_EQUALIZER_OK,
		      "full-scale feedback owner did not initialize");
		for (unsigned i = 0; i < 3; ++i) {
			equalizer.feedback.coefficient_a[i] = trial < 25 ?
				edges[trial % 5] : next_sample(&random);
			equalizer.feedback.coefficient_b[i] = trial < 25 ?
				edges[trial / 5] : next_sample(&random);
		}
		for (unsigned i = 0; i < 4; ++i) {
			equalizer.feedback.in_phase_history[i] = trial < 25 ?
				edges[(trial + i) % 5] : next_sample(&random);
			equalizer.feedback.quadrature_history[i] = trial < 25 ?
				edges[(trial / 5 + i) % 5] : next_sample(&random);
		}
		saved = equalizer.feedback;
		/* Widen the complete complex convolution, then wrap once. Read
		 * history in age order, independently of the filter's tap walk. */
		for (unsigned age = 0; age < 3; ++age) {
			int64_t a = saved.coefficient_a[2 - age];
			int64_t b = saved.coefficient_b[2 - age];
			int64_t x = saved.in_phase_history[age];
			int64_t y = saved.quadrature_history[age];

			real += a * x - b * y;
			imaginary += b * x + a * y;
		}
		CHECK(push_zero_symbol(&equalizer) &&
		      v34_adaptive_equalizer_observe(&equalizer, &observation) ==
			      V34_ADAPTIVE_EQUALIZER_OK &&
		      observation.decision.in_phase == feedback_oracle_sample(real) &&
		      observation.decision.quadrature == feedback_oracle_sample(imaginary),
		      "full-scale feedback projection changed at trial %u", trial);
		for (unsigned i = 0; i < 4; ++i) {
			CHECK(equalizer.feedback.in_phase_history[i] ==
				      (i ? saved.in_phase_history[i - 1] : 0) &&
			      equalizer.feedback.quadrature_history[i] ==
				      (i ? saved.quadrature_history[i - 1] : 0),
			      "feedback history advanced incorrectly at trial %u", trial);
		}
		CHECK(memcmp(equalizer.feedback.coefficient_a, saved.coefficient_a,
			     sizeof(saved.coefficient_a)) == 0 &&
		      memcmp(equalizer.feedback.coefficient_b, saved.coefficient_b,
			     sizeof(saved.coefficient_b)) == 0,
		      "frozen feedback coefficients changed");
	}
	return true;
}

static bool test_feedback_and_discard_lifecycle(void)
{
	struct v34_adaptive_equalizer equalizer;
	struct v34_adaptive_equalizer saved;
	struct v34_adaptive_equalizer_feedback learned;
	struct v34_adaptive_equalizer_feedback applied;
	struct v34_adaptive_equalizer_observation observation;
	struct v34_adaptive_equalizer_observation changed;
	struct v34_adaptive_equalizer_decision decision;

	CHECK(v34_adaptive_equalizer_init(&equalizer) ==
		      V34_ADAPTIVE_EQUALIZER_OK &&
	      push_zero_symbol(&equalizer) &&
	      v34_adaptive_equalizer_observe(&equalizer, &observation) ==
		      V34_ADAPTIVE_EQUALIZER_OK,
	      "discard owner did not produce an observation");
	changed = observation;
	changed.epoch++;
	saved = equalizer;
	CHECK(v34_adaptive_equalizer_discard(&equalizer, &changed) ==
		      V34_ADAPTIVE_EQUALIZER_INVALID_ARGUMENT &&
	      v34_adaptive_equalizer_push(
		      &equalizer, (struct v34_analytic_sample){ 1, 2 }) ==
		      V34_ADAPTIVE_EQUALIZER_INVALID_STATE &&
	      v34_adaptive_equalizer_set_feedback_mode(
		      &equalizer, V34_ADAPTIVE_EQUALIZER_FEEDBACK_LEARN) ==
		      V34_ADAPTIVE_EQUALIZER_INVALID_STATE &&
	      memcmp(&equalizer, &saved, sizeof(equalizer)) == 0,
	      "pending observation did not remain transactional");
	CHECK(v34_adaptive_equalizer_discard(&equalizer, &observation) ==
		      V34_ADAPTIVE_EQUALIZER_OK &&
	      !equalizer.decision_pending,
	      "pre-feedback discard failed");
	CHECK(v34_adaptive_equalizer_set_feedback_mode(
		      &equalizer, V34_ADAPTIVE_EQUALIZER_FEEDBACK_LEARN) ==
		      V34_ADAPTIVE_EQUALIZER_OK &&
	      v34_adaptive_equalizer_set_feedback_mode(
		      &equalizer, V34_ADAPTIVE_EQUALIZER_FEEDBACK_LEARN) ==
		      V34_ADAPTIVE_EQUALIZER_OK,
	      "feedback learning was not idempotent");
	CHECK(push_zero_symbol(&equalizer), "feedback input failed");
	CHECK(v34_adaptive_equalizer_observe(&equalizer, &observation) ==
		      V34_ADAPTIVE_EQUALIZER_OK,
	      "feedback observation failed");
	CHECK(v34_adaptive_equalizer_discard(&equalizer, &observation) ==
		      V34_ADAPTIVE_EQUALIZER_OK &&
	      !equalizer.decision_pending,
	      "learn-only observation was not discardable");
	CHECK(push_zero_symbol(&equalizer) &&
	      v34_adaptive_equalizer_observe(&equalizer, &observation) ==
		      V34_ADAPTIVE_EQUALIZER_OK,
	      "feedback learning observation failed");
	decision = (struct v34_adaptive_equalizer_decision){
		.observation = observation,
		.target = { 2289, -2290 },
		.adaptation = V34_ADAPTIVE_EQUALIZER_SKIP,
	};
	CHECK(v34_adaptive_equalizer_commit(&equalizer, &decision) ==
		      V34_ADAPTIVE_EQUALIZER_OK,
	      "feedback-only commit failed");
	learned = equalizer.feedback;
	CHECK(v34_adaptive_equalizer_set_feedback_mode(
		      &equalizer, V34_ADAPTIVE_EQUALIZER_FEEDBACK_APPLY) ==
		      V34_ADAPTIVE_EQUALIZER_OK &&
	      memcmp(&equalizer.feedback, &learned, sizeof(learned)) == 0 &&
	      push_zero_symbol(&equalizer) &&
	      v34_adaptive_equalizer_observe(&equalizer, &observation) ==
		      V34_ADAPTIVE_EQUALIZER_OK,
	      "feedback apply transition discarded learned state");
	applied = equalizer.feedback;
	saved = equalizer;
	CHECK(v34_adaptive_equalizer_discard(&equalizer, &observation) ==
		      V34_ADAPTIVE_EQUALIZER_INVALID_STATE &&
	      memcmp(&equalizer, &saved, sizeof(equalizer)) == 0,
	      "applied feedback observation was discardable");
	decision = (struct v34_adaptive_equalizer_decision){
		.observation = observation,
		.target = { 2289, -2290 },
		.adaptation = V34_ADAPTIVE_EQUALIZER_SKIP,
	};
	CHECK(v34_adaptive_equalizer_commit(&equalizer, &decision) ==
		      V34_ADAPTIVE_EQUALIZER_OK &&
	      memcmp(&equalizer.feedback, &applied, sizeof(applied)) == 0 &&
	      v34_adaptive_equalizer_set_feedback_mode(
		      &equalizer, V34_ADAPTIVE_EQUALIZER_FEEDBACK_NONE) ==
		      V34_ADAPTIVE_EQUALIZER_OK,
	      "frozen feedback application did not complete");
	return true;
}

static bool test_discard_lineage_zero(void)
{
	struct v34_adaptive_equalizer equalizer;
	struct v34_adaptive_equalizer saved;
	struct v34_adaptive_equalizer_observation observation;

	CHECK(v34_adaptive_equalizer_init(&equalizer) ==
		      V34_ADAPTIVE_EQUALIZER_OK &&
	      push_zero_symbol(&equalizer) &&
	      v34_adaptive_equalizer_observe(&equalizer, &observation) ==
		      V34_ADAPTIVE_EQUALIZER_OK,
	      "zero-lineage discard fixture failed");
	/* This valid lineage makes lineage_tag(lineage, 4) wrap to zero. */
	equalizer.lineage = UINT64_C(0x3a59415ebfdaf0b6);
	equalizer.pending_observation.lineage = equalizer.lineage;
	observation.lineage = equalizer.lineage;
	CHECK(v34_adaptive_equalizer_is_valid(&equalizer),
	      "zero-lineage discard fixture is invalid");
	saved = equalizer;
	CHECK(v34_adaptive_equalizer_discard(&equalizer, &observation) ==
		      V34_ADAPTIVE_EQUALIZER_INVALID_STATE &&
	      memcmp(&equalizer, &saved, sizeof(equalizer)) == 0,
	      "zero-lineage discard changed the owner");
	return true;
}

static bool test_feedback_training_boundary(void)
{
	struct v34_adaptive_equalizer equalizer;
	struct v34_adaptive_equalizer saved;
	struct v34_q14_coefficient coefficients[V34_PRECODER_TAP_COUNT];
	struct v34_q14_coefficient saved_coefficients[V34_PRECODER_TAP_COUNT];
	static const int16_t coefficient_a[] = { 101, -202, 303 };
	static const int16_t coefficient_b[] = { -404, 505, -606 };

	memset(coefficients, 0x5a, sizeof(coefficients));
	saved_coefficients[0] = coefficients[0];
	saved_coefficients[1] = coefficients[1];
	saved_coefficients[2] = coefficients[2];
	CHECK(v34_adaptive_equalizer_init(&equalizer) ==
		      V34_ADAPTIVE_EQUALIZER_OK &&
	      v34_adaptive_equalizer_set_feedback_mode(
		      &equalizer, V34_ADAPTIVE_EQUALIZER_FEEDBACK_LEARN) ==
		      V34_ADAPTIVE_EQUALIZER_OK,
	      "feedback boundary owner did not initialize");
	memcpy(equalizer.feedback.coefficient_a, coefficient_a,
	       sizeof(coefficient_a));
	memcpy(equalizer.feedback.coefficient_b, coefficient_b,
	       sizeof(coefficient_b));
	for (size_t index = 0; index < 4; ++index) {
		equalizer.feedback.in_phase_history[index] =
			(int16_t)(index + 1u);
		equalizer.feedback.quadrature_history[index] =
			(int16_t)(10u + index);
	}
	CHECK(v34_adaptive_equalizer_push(
		      &equalizer, (struct v34_analytic_sample){ 7, -9 }) ==
		      V34_ADAPTIVE_EQUALIZER_OK &&
	      v34_adaptive_equalizer_finish_feedback_training(
		      &equalizer, coefficients) ==
		      V34_ADAPTIVE_EQUALIZER_OK,
	      "feedback boundary did not freeze a partial symbol");
	CHECK(coefficients[0].real == coefficient_a[2] &&
	      coefficients[0].imaginary == coefficient_b[2] &&
	      coefficients[1].real == coefficient_a[1] &&
	      coefficients[1].imaginary == coefficient_b[1] &&
	      coefficients[2].real == coefficient_a[0] &&
	      coefficients[2].imaginary == coefficient_b[0] &&
	      equalizer.feedback_mode ==
		      V34_ADAPTIVE_EQUALIZER_FEEDBACK_NONE &&
	      equalizer.inputs_since_observation == 1u &&
	      memcmp(equalizer.feedback.coefficient_a, coefficient_a,
		     sizeof(coefficient_a)) == 0 &&
	      memcmp(equalizer.feedback.coefficient_b, coefficient_b,
		     sizeof(coefficient_b)) == 0 &&
	      memcmp(equalizer.feedback.in_phase_history,
		     (const int16_t[4]){ 0 },
		     sizeof(equalizer.feedback.in_phase_history)) == 0 &&
	      memcmp(equalizer.feedback.quadrature_history,
		     (const int16_t[4]){ 0 },
		     sizeof(equalizer.feedback.quadrature_history)) == 0,
	      "feedback boundary changed coefficients or retained history");
	saved = equalizer;
	saved_coefficients[0] = coefficients[0];
	saved_coefficients[1] = coefficients[1];
	saved_coefficients[2] = coefficients[2];
	CHECK(v34_adaptive_equalizer_finish_feedback_training(
		      &equalizer, coefficients) ==
		      V34_ADAPTIVE_EQUALIZER_INVALID_STATE &&
	      v34_adaptive_equalizer_finish_feedback_training(
		      &equalizer, NULL) ==
		      V34_ADAPTIVE_EQUALIZER_INVALID_ARGUMENT &&
	      v34_adaptive_equalizer_finish_feedback_training(
		      &equalizer,
		      (struct v34_q14_coefficient *)&equalizer) ==
		      V34_ADAPTIVE_EQUALIZER_INVALID_ARGUMENT &&
	      memcmp(&equalizer, &saved, sizeof(equalizer)) == 0 &&
	      memcmp(coefficients, saved_coefficients,
		     sizeof(coefficients)) == 0,
	      "feedback boundary rejection changed state or output");
	return true;
}

static bool test_tracking_boundary(void)
{
	struct v34_adaptive_equalizer equalizer;
	struct v34_adaptive_equalizer expected;
	uint64_t before_lineage;

	CHECK(v34_adaptive_equalizer_init(&equalizer) ==
		      V34_ADAPTIVE_EQUALIZER_OK &&
	      v34_adaptive_equalizer_set_feedback_mode(
		      &equalizer, V34_ADAPTIVE_EQUALIZER_FEEDBACK_LEARN) ==
		      V34_ADAPTIVE_EQUALIZER_OK,
	      "center-boundary owner did not initialize");
	for (size_t tap = 0; tap < V34_EQUALIZER_TAPS; ++tap) {
		equalizer.equalizer.in_phase_coefficient[tap] =
			(int16_t)(101 + tap);
		equalizer.equalizer.quadrature_coefficient[tap] =
			(int16_t)(-201 - (int)tap);
		equalizer.equalizer.in_phase_residue[tap] =
			(uint16_t)(301 + tap);
		equalizer.equalizer.quadrature_residue[tap] =
			(uint16_t)(401 + tap);
	}
	for (size_t tap = 0; tap < 3; ++tap) {
		equalizer.feedback.coefficient_a[tap] =
			(int16_t)(503 + (int)tap);
		equalizer.feedback.coefficient_b[tap] =
			(int16_t)(-509 - (int)tap);
	}
	for (size_t sample = 0; sample < 4; ++sample) {
		equalizer.feedback.in_phase_history[sample] =
			(int16_t)(-521 - (int)sample);
		equalizer.feedback.quadrature_history[sample] =
			(int16_t)(523 + (int)sample);
	}
	equalizer.quality = (struct v34_adaptive_equalizer_quality){
		.recent_error_blocks = { [3] = 593 },
		.recent_symbol_count = 512,
		.recent_decision_metric = 587,
		.decision_accumulator = 601,
		.feedback_accumulator = 607,
		.reference_accumulator = 613,
		.reference_metric = 617,
		.window_position = 619,
		.decision_metric = 631,
		.feedback_metric = 641,
	};
	expected = equalizer;
	memset(&expected.quality, 0, sizeof(expected.quality));
	memset(expected.feedback.coefficient_a, 0,
	       sizeof(expected.feedback.coefficient_a));
	memset(expected.feedback.coefficient_b, 0,
	       sizeof(expected.feedback.coefficient_b));
	for (size_t tap = V34_EQUALIZER_CENTER_FIRST;
	     tap < V34_EQUALIZER_CENTER_FIRST + V34_EQUALIZER_CENTER_TAPS;
	     ++tap) {
		expected.equalizer.in_phase_coefficient[tap] = 0;
		expected.equalizer.quadrature_coefficient[tap] = 0;
	}
	before_lineage = equalizer.lineage;
	CHECK(v34_adaptive_equalizer_begin_tracking(&equalizer) ==
		      V34_ADAPTIVE_EQUALIZER_OK &&
	      equalizer.lineage != before_lineage,
	      "center-boundary transition failed");
	expected.lineage = equalizer.lineage;
	CHECK(memcmp(&equalizer, &expected, sizeof(equalizer)) == 0 &&
	      v34_adaptive_equalizer_is_valid(&equalizer),
	      "tracking boundary changed retained equalizer state");
	CHECK(v34_adaptive_equalizer_begin_tracking(&equalizer) ==
		      V34_ADAPTIVE_EQUALIZER_OK,
	      "repeated center-boundary transition failed");
	return true;
}

static bool test_decision_quality_window(void)
{
	struct v34_adaptive_equalizer equalizer;
	struct v34_adaptive_equalizer_observation observation;
	struct v34_adaptive_equalizer_decision decision;
	enum v34_adaptive_equalizer_result result;
	const struct v34_analytic_sample target = { 1000, 0 };

	CHECK(v34_adaptive_equalizer_init(&equalizer) ==
		      V34_ADAPTIVE_EQUALIZER_OK &&
	      v34_adaptive_equalizer_set_feedback_mode(
		      &equalizer, V34_ADAPTIVE_EQUALIZER_FEEDBACK_LEARN) ==
		      V34_ADAPTIVE_EQUALIZER_OK &&
	      push_zero_symbol(&equalizer) &&
	      v34_adaptive_equalizer_observe(&equalizer, &observation) ==
		      V34_ADAPTIVE_EQUALIZER_OK,
	      "quality owner did not initialize");
	decision = (struct v34_adaptive_equalizer_decision){
		.observation = observation,
		.target = target,
		.adaptation = V34_ADAPTIVE_EQUALIZER_SKIP,
	};
	result = v34_adaptive_equalizer_commit(&equalizer, &decision);
	CHECK(result == V34_ADAPTIVE_EQUALIZER_OK &&
	      equalizer.quality.feedback_accumulator == UINT32_C(1002001) &&
	      equalizer.quality.decision_accumulator == 0 &&
	      equalizer.quality.reference_accumulator == 0 &&
	      equalizer.quality.window_position == 0 &&
	      equalizer.quality.recent_symbol_count == 0u,
	      "skipped decision changed the quality window: %d/%u/%u/%u/%u at %d,%d",
	      result, equalizer.quality.feedback_accumulator,
	      equalizer.quality.decision_accumulator,
	      equalizer.quality.reference_accumulator,
	      equalizer.quality.window_position, observation.decision.in_phase,
	      observation.decision.quadrature);
	CHECK(v34_adaptive_equalizer_set_feedback_mode(
		      &equalizer, V34_ADAPTIVE_EQUALIZER_FEEDBACK_NONE) ==
		      V34_ADAPTIVE_EQUALIZER_OK,
	      "quality owner did not freeze feedback learning");

	CHECK(V34_ADAPTIVE_EQUALIZER_QUALITY_SYMBOLS == 1024,
	      "quality window length changed");
	for (size_t symbol = 0;
	     symbol < (size_t)V34_ADAPTIVE_EQUALIZER_QUALITY_SYMBOLS;
	     ++symbol) {
		CHECK(push_zero_symbol(&equalizer) &&
		      v34_adaptive_equalizer_observe(
			      &equalizer, &observation) ==
			      V34_ADAPTIVE_EQUALIZER_OK,
		      "quality observation failed at %zu", symbol);
		decision = (struct v34_adaptive_equalizer_decision){
			.observation = observation,
			.target = target,
			.decoder_power_q15 = 0x4000,
			.adaptation = V34_ADAPTIVE_EQUALIZER_FULL,
		};
		CHECK(v34_adaptive_equalizer_commit(&equalizer, &decision) ==
			      V34_ADAPTIVE_EQUALIZER_OK,
		      "quality commit failed at %zu", symbol);
		CHECK(equalizer.quality.recent_symbol_count ==
			      (symbol < 512u ? symbol + 1u : 512u) &&
		      equalizer.quality.recent_decision_metric ==
			      (symbol < 511u ? 0 : 15625),
		      "recent quality published before a complete window at %zu", symbol);
	}
	CHECK(equalizer.quality.decision_metric == 15625 &&
	      equalizer.quality.recent_symbol_count == 512u &&
	      equalizer.quality.recent_decision_metric == 15625 &&
	      equalizer.quality.feedback_metric == 15 &&
	      equalizer.quality.reference_metric == UINT32_C(15624) &&
	      equalizer.quality.window_position == 0 &&
	      equalizer.quality.decision_accumulator == 0 &&
	      equalizer.quality.feedback_accumulator == 0 &&
	      equalizer.quality.reference_accumulator == 0,
	      "published quality window changed");
	/* A 512-symbol estimate advances every 64 symbols without waiting for
	 * the old 1024-symbol window to wrap. A short tail never replaces it. */
	for (unsigned int symbol = 1u; symbol <= 1024u; ++symbol) {
		unsigned int quiet_symbols = symbol / 64u * 64u;
		int16_t expected = symbol <= 512u ?
			(int16_t)((uint64_t)(512u - quiet_symbols) * 1000000u >> 15) :
			symbol < 576u ? 0 : INT16_MAX;
		CHECK(push_zero_symbol(&equalizer) &&
		      v34_adaptive_equalizer_observe(&equalizer, &observation) ==
			      V34_ADAPTIVE_EQUALIZER_OK,
		      "rolling quality observation failed at %u", symbol);
		decision = (struct v34_adaptive_equalizer_decision){
			.observation = observation,
			.target = symbol <= 512u ?
				(struct v34_analytic_sample){ 0, 0 } :
				(struct v34_analytic_sample){ INT16_MAX, INT16_MAX },
			.decoder_power_q15 = 0x4000,
			.adaptation = V34_ADAPTIVE_EQUALIZER_FULL,
		};
		CHECK(v34_adaptive_equalizer_commit(&equalizer, &decision) ==
			      V34_ADAPTIVE_EQUALIZER_OK &&
		      equalizer.quality.recent_decision_metric == expected &&
		      (symbol == 1024u || equalizer.quality.decision_metric == 15625),
		      "rolling quality changed at %u: %d, expected %d",
		      symbol, equalizer.quality.recent_decision_metric, expected);
	}
	CHECK(v34_adaptive_equalizer_begin_tracking(&equalizer) ==
		      V34_ADAPTIVE_EQUALIZER_OK &&
	      memcmp(&equalizer.quality,
		     &(const struct v34_adaptive_equalizer_quality){ 0 },
		     sizeof(equalizer.quality)) == 0,
	      "training boundary retained an old quality window");
	return true;
}

static bool test_transactionality(void)
{
	struct v34_adaptive_equalizer equalizer;
	struct v34_adaptive_equalizer copied;
	struct v34_adaptive_equalizer saved;
	struct v34_adaptive_equalizer invalid;
	struct v34_adaptive_equalizer_observation observation = {
		.decision = { 17, -23 },
		.epoch = 31,
		.lineage = 47,
	};
	struct v34_adaptive_equalizer_observation saved_observation =
		observation;
	struct v34_adaptive_equalizer_decision decision;

	memset(&invalid, 0, sizeof(invalid));
	CHECK(v34_adaptive_equalizer_init(NULL) ==
		      V34_ADAPTIVE_EQUALIZER_INVALID_ARGUMENT &&
	      !v34_adaptive_equalizer_is_valid(NULL) &&
	      !v34_adaptive_equalizer_is_valid(&invalid) &&
	      v34_adaptive_equalizer_push(
		      &invalid, (struct v34_analytic_sample){ 1, 2 }) ==
		      V34_ADAPTIVE_EQUALIZER_INVALID_STATE &&
	      v34_adaptive_equalizer_observe(&invalid, &observation) ==
		      V34_ADAPTIVE_EQUALIZER_INVALID_STATE &&
	      v34_adaptive_equalizer_observations_are_equal(
		      &observation, &saved_observation),
	      "invalid owner handling changed");

	CHECK(v34_adaptive_equalizer_init(&equalizer) ==
		      V34_ADAPTIVE_EQUALIZER_OK,
	      "transaction owner did not initialize");
	saved = equalizer;
	CHECK(v34_adaptive_equalizer_set_feedback_mode(
		      &equalizer,
		      (enum v34_adaptive_equalizer_feedback_mode)-1) ==
		      V34_ADAPTIVE_EQUALIZER_INVALID_ARGUMENT &&
	      v34_adaptive_equalizer_set_feedback_mode(
		      &equalizer,
		      (enum v34_adaptive_equalizer_feedback_mode)0x100) ==
		      V34_ADAPTIVE_EQUALIZER_INVALID_ARGUMENT &&
	      v34_adaptive_equalizer_observe(&equalizer, NULL) ==
		      V34_ADAPTIVE_EQUALIZER_INVALID_ARGUMENT &&
	      v34_adaptive_equalizer_observe(
		      &equalizer,
		      (struct v34_adaptive_equalizer_observation *)&equalizer) ==
		      V34_ADAPTIVE_EQUALIZER_INVALID_ARGUMENT &&
	      v34_adaptive_equalizer_commit(&equalizer, NULL) ==
		      V34_ADAPTIVE_EQUALIZER_INVALID_ARGUMENT &&
	      v34_adaptive_equalizer_discard(&equalizer, NULL) ==
		      V34_ADAPTIVE_EQUALIZER_INVALID_ARGUMENT &&
	      v34_adaptive_equalizer_begin_tracking(NULL) ==
		      V34_ADAPTIVE_EQUALIZER_INVALID_STATE &&
	      memcmp(&equalizer, &saved, sizeof(equalizer)) == 0,
	      "null/alias preflight changed the owner");
	CHECK(v34_adaptive_equalizer_observe(&equalizer, &observation) ==
		      V34_ADAPTIVE_EQUALIZER_INVALID_STATE &&
	      v34_adaptive_equalizer_push(
		      &equalizer, (struct v34_analytic_sample){ 1, 2 }) ==
		      V34_ADAPTIVE_EQUALIZER_OK,
	      "two-input cadence accepted an empty symbol");
	saved = equalizer;
	CHECK(v34_adaptive_equalizer_set_feedback_mode(
		      &equalizer, V34_ADAPTIVE_EQUALIZER_FEEDBACK_LEARN) ==
		      V34_ADAPTIVE_EQUALIZER_INVALID_STATE &&
	      v34_adaptive_equalizer_begin_tracking(&equalizer) ==
		      V34_ADAPTIVE_EQUALIZER_INVALID_STATE &&
	      memcmp(&equalizer, &saved, sizeof(equalizer)) == 0 &&
	      v34_adaptive_equalizer_observe(&equalizer, &observation) ==
		      V34_ADAPTIVE_EQUALIZER_INVALID_STATE &&
	      v34_adaptive_equalizer_push(
		      &equalizer, (struct v34_analytic_sample){ 3, 4 }) ==
		      V34_ADAPTIVE_EQUALIZER_OK,
	      "partial symbol changed feedback or produced a decision");
	saved = equalizer;
	CHECK(v34_adaptive_equalizer_push(
		      &equalizer, (struct v34_analytic_sample){ 5, 6 }) ==
		      V34_ADAPTIVE_EQUALIZER_INVALID_STATE &&
	      memcmp(&equalizer, &saved, sizeof(equalizer)) == 0,
	      "two-input cadence accepted an extra input");
	CHECK(v34_adaptive_equalizer_observe(&equalizer, &observation) ==
		      V34_ADAPTIVE_EQUALIZER_OK,
	      "transaction observation failed");
	decision = zero_decision(&observation);
	saved = equalizer;
	decision.observation.lineage++;
	CHECK(v34_adaptive_equalizer_commit(&equalizer, &decision) ==
		      V34_ADAPTIVE_EQUALIZER_INVALID_ARGUMENT &&
	      v34_adaptive_equalizer_begin_tracking(&equalizer) ==
		      V34_ADAPTIVE_EQUALIZER_INVALID_STATE &&
	      memcmp(&equalizer, &saved, sizeof(equalizer)) == 0,
	      "tampered observation changed the owner");
	decision = zero_decision(&observation);
	decision.adaptation = (enum v34_adaptive_equalizer_adaptation)99;
	CHECK(v34_adaptive_equalizer_commit(&equalizer, &decision) ==
		      V34_ADAPTIVE_EQUALIZER_INVALID_ARGUMENT &&
	      memcmp(&equalizer, &saved, sizeof(equalizer)) == 0,
	      "invalid adaptation changed the owner");
	decision = zero_decision(&observation);
	decision.decoder_power_q15 = 0;
	CHECK(v34_adaptive_equalizer_commit(&equalizer, &decision) ==
		      V34_ADAPTIVE_EQUALIZER_INVALID_ARGUMENT &&
	      memcmp(&equalizer, &saved, sizeof(equalizer)) == 0,
	      "invalid decoder power changed the owner");
	CHECK(v34_adaptive_equalizer_commit(
		      &equalizer,
		      (const struct v34_adaptive_equalizer_decision *)&equalizer) ==
		      V34_ADAPTIVE_EQUALIZER_INVALID_ARGUMENT &&
	      v34_adaptive_equalizer_discard(
		      &equalizer,
		      (const struct v34_adaptive_equalizer_observation *)&equalizer) ==
		      V34_ADAPTIVE_EQUALIZER_INVALID_ARGUMENT &&
	      memcmp(&equalizer, &saved, sizeof(equalizer)) == 0,
	      "pending alias rejection changed the owner");

	copied = equalizer;
	decision = zero_decision(&observation);
	CHECK(v34_adaptive_equalizer_commit(&equalizer, &decision) ==
		      V34_ADAPTIVE_EQUALIZER_OK &&
	      v34_adaptive_equalizer_commit(&copied, &decision) ==
		      V34_ADAPTIVE_EQUALIZER_OK &&
	      memcmp(&equalizer, &copied, sizeof(equalizer)) == 0,
	      "byte-identical copied owner lost compatible lineage");

	invalid = equalizer;
	invalid.equalizer.index = V34_EQUALIZER_TAPS;
	saved = invalid;
	CHECK(!v34_adaptive_equalizer_is_valid(&invalid) &&
	      v34_adaptive_equalizer_set_feedback_mode(
		      &invalid, V34_ADAPTIVE_EQUALIZER_FEEDBACK_LEARN) ==
		      V34_ADAPTIVE_EQUALIZER_INVALID_STATE &&
	      v34_adaptive_equalizer_push(
		      &invalid, (struct v34_analytic_sample){ 3, 4 }) ==
		      V34_ADAPTIVE_EQUALIZER_INVALID_STATE &&
	      memcmp(&invalid, &saved, sizeof(invalid)) == 0,
	      "corrupt owner rejection was not transactional");
	invalid = equalizer;
	invalid.rotate_in_phase = 0x4006;
	CHECK(!v34_adaptive_equalizer_is_valid(&invalid),
	      "out-of-domain carrier rotation was accepted");
	invalid = equalizer;
	invalid.quality.window_position =
		V34_ADAPTIVE_EQUALIZER_QUALITY_SYMBOLS;
	CHECK(!v34_adaptive_equalizer_is_valid(&invalid),
	      "out-of-domain quality cursor was accepted");
	invalid = equalizer;
	invalid.quality.decision_metric = -1;
	CHECK(!v34_adaptive_equalizer_is_valid(&invalid),
	      "negative published quality metric was accepted");
	invalid = equalizer;
	invalid.quality.recent_symbol_count =
		V34_ADAPTIVE_EQUALIZER_RECENT_SYMBOLS + 1u;
	CHECK(!v34_adaptive_equalizer_is_valid(&invalid),
	      "out-of-domain recent quality window was accepted");
	invalid = equalizer;
	invalid.quality.recent_decision_metric = -1;
	CHECK(!v34_adaptive_equalizer_is_valid(&invalid),
	      "negative recent quality metric was accepted");
	return true;
}

static enum v34_data_quality_result publish_quality_window(
	struct v34_data_quality *quality,
	struct v34_adaptive_equalizer_quality *measurement,
	int16_t metric)
{
	measurement->window_position = 1;
	if (v34_data_quality_observe(quality, measurement) !=
	    V34_DATA_QUALITY_OK)
		return V34_DATA_QUALITY_INVALID_STATE;
	measurement->decision_metric = metric;
	measurement->window_position = 0;
	return v34_data_quality_observe(quality, measurement);
}

static bool test_data_quality_policy(void)
{
	static const uint8_t maximum_rates[] = { 9, 11, 11, 12, 13, 14 };
	static const struct {
		enum v34_symbol_rate symbol_rate;
		uint8_t rate_units;
		int16_t threshold;
	} profiles[] = {
		{ V34_SYMBOL_RATE_2400, 9, 142 },
		{ V34_SYMBOL_RATE_2743, 10, 165 },
		{ V34_SYMBOL_RATE_2800, 11, 89 },
		{ V34_SYMBOL_RATE_3000, 12, 78 },
		{ V34_SYMBOL_RATE_3200, 13, 80 },
		{ V34_SYMBOL_RATE_3429, 14, 71 },
	};
	struct v34_adaptive_equalizer_quality measurement = {
		.decision_metric = 100,
		.window_position = 1,
	};
	struct v34_data_quality quality;
	uint8_t selected_rate;
	uint64_t profile_hash = UINT64_C(14695981039346656037);

	for (size_t index = 0; index < sizeof(profiles) / sizeof(profiles[0]);
	     ++index) {
		CHECK(v34_data_quality_init(&quality) == V34_DATA_QUALITY_OK &&
		      v34_data_quality_start(
			      &quality, profiles[index].symbol_rate,
			      profiles[index].rate_units, &measurement, 1) ==
			      V34_DATA_QUALITY_OK &&
		      quality.threshold == profiles[index].threshold,
		      "quality threshold changed at profile %zu: %d", index,
		      quality.threshold);
	}
	CHECK(v34_data_quality_init(&quality) == V34_DATA_QUALITY_OK,
	      "quality profile digest did not initialize");
	for (size_t index = 0; index < sizeof(maximum_rates); ++index) {
		uint8_t minimum_rate = index == V34_SYMBOL_RATE_2400 ? 1u : 2u;

		for (uint8_t rate = minimum_rate; rate <= maximum_rates[index];
		     ++rate) {
			CHECK(v34_data_quality_start(
				      &quality, (enum v34_symbol_rate)index, rate,
				      &measurement, 1) == V34_DATA_QUALITY_OK,
			      "quality profile rejected %zu/%u", index, rate);
			profile_hash = hash_word(
				profile_hash, (uint16_t)quality.threshold);
		}
	}
	CHECK(profile_hash == UINT64_C(0x7bcefe06f5c60098),
	      "quality profile digest changed: 0x%016" PRIx64, profile_hash);
	measurement.decision_metric = 49;
	CHECK(v34_data_quality_select_receive_rate(
		      V34_SYMBOL_RATE_3429, 0x3fffu, 14, &measurement,
		      &selected_rate) == V34_DATA_QUALITY_OK &&
	      selected_rate == 14,
	      "sub-threshold training did not retain 33600: %u",
	      selected_rate);
	measurement.decision_metric = 50;
	CHECK(v34_data_quality_select_receive_rate(
		      V34_SYMBOL_RATE_3429, 0x3fffu, 14, &measurement,
		      &selected_rate) == V34_DATA_QUALITY_OK &&
	      selected_rate == 13,
	      "threshold boundary did not step down to 31200: %u",
	      selected_rate);
	measurement.decision_metric = 88;
	CHECK(v34_data_quality_select_receive_rate(
		      V34_SYMBOL_RATE_3429, 0x3fffu, 14, &measurement,
		      &selected_rate) == V34_DATA_QUALITY_OK &&
	      selected_rate == 12,
	      "retained live metric did not select 28800: %u",
	      selected_rate);
	measurement.decision_metric = 50;
	CHECK(v34_data_quality_select_receive_rate(
		      V34_SYMBOL_RATE_3429,
		      (uint16_t)((1u << 13u) | (1u << 11u)), 14,
		      &measurement, &selected_rate) == V34_DATA_QUALITY_OK &&
	      selected_rate == 12,
	      "sparse capability mask did not skip rate 13: %u",
	      selected_rate);
	measurement.decision_metric = INT16_MAX;
	CHECK(v34_data_quality_select_receive_rate(
		      V34_SYMBOL_RATE_2400, 0x01ffu, 9, &measurement,
		      &selected_rate) == V34_DATA_QUALITY_OK &&
	      selected_rate == 1,
	      "poor training did not retain the minimum rate: %u",
	      selected_rate);
	measurement.decision_metric = 100;
	CHECK(v34_data_quality_init(&quality) == V34_DATA_QUALITY_OK &&
	      v34_data_quality_start(
		      &quality, V34_SYMBOL_RATE_2400, 1, &measurement, 1) ==
		      V34_DATA_QUALITY_OK &&
	      quality.threshold == 840,
	      "minimum-rate training baseline changed: %d", quality.threshold);
	measurement.decision_metric = INT16_MAX / 8;
	CHECK(v34_data_quality_start(
		      &quality, V34_SYMBOL_RATE_2400, 1, &measurement, 1) ==
		      V34_DATA_QUALITY_OK && quality.threshold == INT16_MAX &&
	      v34_data_quality_is_valid(&quality),
	      "minimum-rate threshold headroom overflowed");
	measurement.decision_metric = 5000;
	CHECK(v34_data_quality_start(
		      &quality, V34_SYMBOL_RATE_2743, 2, &measurement, 1) ==
		      V34_DATA_QUALITY_OK &&
	      quality.threshold == 0 && quality.active,
	      "saturated minimum-rate baseline was not disabled");
	CHECK(publish_quality_window(&quality, &measurement, INT16_MAX) ==
		      V34_DATA_QUALITY_OK &&
	      quality.recovery.bad_windows == 0,
	      "disabled minimum-rate quality requested recovery");
	measurement.decision_metric = 0;
	measurement.window_position = 1;
	measurement.decision_metric = 109;
	CHECK(v34_data_quality_init(&quality) == V34_DATA_QUALITY_OK &&
	      v34_data_quality_start(
		      &quality, V34_SYMBOL_RATE_3429, 12, &measurement, 1) ==
		      V34_DATA_QUALITY_OK &&
	      quality.baseline == 109 && quality.threshold == 177 &&
	      !quality.timing_frozen &&
	      publish_quality_window(&quality, &measurement, 133) ==
		      V34_DATA_QUALITY_OK &&
	      !quality.timing_frozen &&
	      publish_quality_window(&quality, &measurement, 134) ==
		      V34_DATA_QUALITY_FREEZE_TIMING &&
	      quality.timing_frozen &&
	      publish_quality_window(&quality, &measurement, 127) ==
		      V34_DATA_QUALITY_OK &&
	      quality.timing_frozen &&
	      publish_quality_window(&quality, &measurement, 126) ==
		      V34_DATA_QUALITY_RESUME_TIMING &&
	      !quality.timing_frozen,
	      "quality timing hysteresis changed");
	measurement.decision_metric = 0;
	measurement.window_position = 1;

	CHECK(v34_data_quality_init(&quality) == V34_DATA_QUALITY_OK &&
	      v34_data_quality_start(
		      &quality, V34_SYMBOL_RATE_3429, 14, &measurement, 1) ==
		      V34_DATA_QUALITY_OK &&
	      quality.threshold == 71,
	      "retained quality profile did not initialize");
	measurement.decision_metric = 192;
	measurement.window_position = 0;
	CHECK(v34_data_quality_observe(&quality, &measurement) ==
		      V34_DATA_QUALITY_FREEZE_TIMING &&
	      quality.recovery.bad_windows == 1 && quality.window_position == 0 &&
	      quality.timing_frozen &&
	      v34_data_quality_observe(&quality, &measurement) ==
		      V34_DATA_QUALITY_OK &&
	      quality.recovery.bad_windows == 1,
	      "one bad window was not consumed exactly once");
	CHECK(publish_quality_window(&quality, &measurement, 66) ==
		      V34_DATA_QUALITY_OK &&
	      quality.recovery.bad_windows == 0,
	      "a recovered window did not clear quality loss");
	for (unsigned int window = 0;
	     window < quality.loss_windows; ++window) {
		enum v34_data_quality_result expected =
			window + 1u == quality.loss_windows ?
				V34_DATA_QUALITY_RETRAIN :
				V34_DATA_QUALITY_OK;

		CHECK(publish_quality_window(&quality, &measurement, 192) ==
			      expected,
		      "quality loss result changed at window %u", window);
	}
	CHECK(!quality.active && quality.recovery.attempts == 1 &&
	      v34_data_quality_is_valid(&quality),
	      "first quality retrain did not preserve a valid budget");

	for (unsigned int retry = 1;
	     retry < V34_DATA_QUALITY_MAX_RETRAINS; ++retry) {
		measurement.window_position = 0;
		CHECK(v34_data_quality_start(
			      &quality, V34_SYMBOL_RATE_3429, 14,
			      &measurement, 1) == V34_DATA_QUALITY_OK,
		      "quality generation %u did not restart", retry);
		for (unsigned int window = 0;
		     window < quality.loss_windows; ++window) {
			enum v34_data_quality_result expected =
				window + 1u == quality.loss_windows ?
					V34_DATA_QUALITY_RETRAIN :
					V34_DATA_QUALITY_OK;

			CHECK(publish_quality_window(
				      &quality, &measurement, 198) == expected,
			      "quality retry %u window %u changed", retry,
			      window);
		}
	}
	CHECK(quality.recovery.attempts == V34_DATA_QUALITY_MAX_RETRAINS &&
	      !quality.active,
	      "quality retrain budget did not saturate");
	measurement.window_position = 1;
	CHECK(v34_data_quality_start(
		      &quality, V34_SYMBOL_RATE_3200, 11, &measurement, 1) ==
		      V34_DATA_QUALITY_OK &&
	      quality.threshold == 220,
	      "26400 quality profile changed");
	for (unsigned int window = 0;
	     window < quality.loss_windows; ++window)
		CHECK(publish_quality_window(&quality, &measurement, 345) ==
			      (window == 0 ?
				       V34_DATA_QUALITY_FREEZE_TIMING :
				       V34_DATA_QUALITY_OK),
		      "exhausted quality budget retriggered at %u", window);
	CHECK(quality.active &&
	      quality.recovery.bad_windows == quality.loss_windows &&
	      publish_quality_window(&quality, &measurement, 166) ==
		      V34_DATA_QUALITY_RESUME_TIMING &&
	      quality.recovery.bad_windows == 0 && !quality.timing_frozen,
	      "exhausted quality monitor did not retain recovery evidence");
	return true;
}

static bool test_data_quality_duration(void)
{
	static const struct {
		unsigned numerator;
		unsigned denominator;
		unsigned windows;
	} rates[] = {
		{ 2400, 1, 5 }, { 19200, 7, 5 }, { 2800, 1, 5 },
		{ 3000, 1, 6 }, { 3200, 1, 6 }, { 24000, 7, 7 },
	};

	for (unsigned stride = 1; stride <= 2; ++stride) {
		for (unsigned rate = 0; rate < ARRAY_LENGTH(rates); ++rate) {
			struct v34_adaptive_equalizer_quality measurement = { 0 };
			struct v34_data_quality quality = { 0 };
			unsigned window_time = V34_ADAPTIVE_EQUALIZER_QUALITY_SYMBOLS *
				1000u * rates[rate].denominator * stride;
			unsigned windows = (rates[rate].windows + stride - 1u) / stride;
			unsigned minimum_time = 1800u * rates[rate].numerator;

			CHECK(v34_data_quality_start(&quality, (enum v34_symbol_rate)rate,
				      9, &measurement, stride) == V34_DATA_QUALITY_OK &&
			      quality.loss_windows == windows &&
			      quality.loss_windows * window_time >= minimum_time &&
			      (quality.loss_windows - 1u) * window_time < minimum_time,
			      "quality delay is not the shortest count covering 1.8 s at rate %u", rate);
			/* A metric just below the new boundary breaks a nearly complete
			 * failure streak; a metric exactly at it starts a fresh streak. */
			for (unsigned pass = 0; pass < 2; ++pass) {
				for (unsigned window = 1; window <= windows; ++window) {
					bool recovered = pass == 0 && window == windows;
					enum v34_data_quality_result result = publish_quality_window(
						&quality, &measurement,
						(int16_t)(quality.threshold - (recovered ? 1 : 0)));

					CHECK(result == (pass == 1 && window == windows ?
						      V34_DATA_QUALITY_RETRAIN :
						      pass == 0 && window == 1 ?
						      V34_DATA_QUALITY_FREEZE_TIMING : V34_DATA_QUALITY_OK),
					      "quality recovery fired at rate %u pass %u window %u",
					      rate, pass, window);
					if (recovered)
						CHECK(quality.recovery.bad_windows == 0,
						      "below-threshold window retained bad credit");
				}
			}
			CHECK(!quality.active && quality.recovery.attempts == 1 &&
			      v34_data_quality_is_valid(&quality),
			      "duration recovery left invalid state at rate %u", rate);
		}
	}
	return true;
}

static bool test_recent_rate_measurement(void)
{
	static const struct {
		uint16_t position;
		uint16_t recent_symbols;
		int16_t published;
		int16_t recent;
		uint8_t expected_rate;
	} cases[] = {
		{ 0u, 0u, 50, 0, 13u },
		{ 511u, 511u, 50, 0, 13u },
		{ 511u, 511u, 49, INT16_MAX, 14u },
		{ 512u, 512u, INT16_MAX, 49, 14u },
		{ 493u, 512u, INT16_MAX, 49, 14u },
		{ 0u, 512u, 0, 50, 13u },
		{ 1023u, 512u, 0, 88, 12u },
		{ 512u, 512u, 0, INT16_MAX, 2u },
	};

	for (size_t index = 0; index < ARRAY_LENGTH(cases); ++index) {
		struct v34_adaptive_equalizer_quality measurement = {
			.window_position = cases[index].position,
			.recent_symbol_count = cases[index].recent_symbols,
			.recent_decision_metric = cases[index].recent,
			.decision_metric = cases[index].published,
		};
		struct v34_adaptive_equalizer_quality saved = measurement;
		struct v34_data_quality online;
		uint8_t selected = 0;

		CHECK(v34_data_quality_select_receive_rate(
			      V34_SYMBOL_RATE_3429, 0x3fffu, 14u,
			      &measurement, &selected) == V34_DATA_QUALITY_OK &&
		      selected == cases[index].expected_rate &&
		      memcmp(&measurement, &saved, sizeof(saved)) == 0,
		      "recent training metric changed at case %zu: rate %u",
		      index, selected);
		CHECK(v34_data_quality_init(&online) == V34_DATA_QUALITY_OK &&
		      v34_data_quality_start(
			      &online, V34_SYMBOL_RATE_3429, 14u,
			      &measurement, 1) == V34_DATA_QUALITY_OK &&
		      online.baseline == cases[index].published,
		      "rate selection published a partial online window");
	}
	return true;
}

static bool test_paired_data_quality_windows(void)
{
	for (uint16_t phase = 0; phase < 2; ++phase) {
		struct v34_adaptive_equalizer_quality measurement = {
			.decision_metric = 40,
			.window_position = (uint16_t)(1022u + phase),
		};
		struct v34_data_quality quality;

		CHECK(v34_data_quality_init(&quality) == V34_DATA_QUALITY_OK &&
		      v34_data_quality_start(
			      &quality, V34_SYMBOL_RATE_3200, 11, &measurement, 1) ==
			      V34_DATA_QUALITY_OK,
		      "paired quality phase %u did not start", phase);
		for (unsigned int window = 0;
		     window < quality.loss_windows; ++window) {
			enum v34_data_quality_result expected = window == 0 ?
				V34_DATA_QUALITY_FREEZE_TIMING :
				(window + 1 == quality.loss_windows ?
				 V34_DATA_QUALITY_RETRAIN : V34_DATA_QUALITY_OK);

			measurement.window_position = (uint16_t)(1022u + phase);
			CHECK(v34_data_quality_observe(&quality, &measurement) ==
				      V34_DATA_QUALITY_OK && quality.recovery.bad_windows == window,
			      "an incomplete paired window changed quality");
			measurement.window_position = phase;
			measurement.decision_metric = 480;
			CHECK(v34_data_quality_observe(&quality, &measurement) == expected,
			      "paired quality phase %u lost completed window %u", phase, window);
			if (expected != V34_DATA_QUALITY_RETRAIN)
				CHECK(v34_data_quality_observe(&quality, &measurement) ==
					      V34_DATA_QUALITY_OK && quality.recovery.bad_windows == window + 1,
				      "paired quality counted a window twice");
		}
		CHECK(!quality.active && quality.recovery.attempts == 1 &&
		      v34_data_quality_is_valid(&quality),
		      "paired quality did not retain its recovery budget");
	}
	return true;
}

static bool test_data_quality_rearm(void)
{
	for (unsigned budget = 1; budget <= V34_DATA_QUALITY_MAX_RETRAINS; ++budget) {
		struct v34_adaptive_equalizer_quality measurement = { .decision_metric = 40 };
		struct v34_data_quality quality = { .recovery.attempts = budget };

		CHECK(v34_data_quality_start(&quality, V34_SYMBOL_RATE_3429, 12,
			      &measurement, 1) == V34_DATA_QUALITY_OK,
		      "rearm fixture did not start");
		for (unsigned window = 1; window <= V34_DATA_QUALITY_REARM_WINDOWS; ++window) {
			measurement.window_position = 1;
			for (unsigned repeat = 0; repeat < 4; ++repeat)
				CHECK(v34_data_quality_observe(&quality, &measurement) ==
					      V34_DATA_QUALITY_OK && quality.recovery.attempts == budget,
				      "partial or unchanged measurement rearmed recovery");
			CHECK(publish_quality_window(&quality, &measurement, 40) ==
				      V34_DATA_QUALITY_OK && quality.recovery.attempts ==
				      (window == V34_DATA_QUALITY_REARM_WINDOWS ? 0 : budget) &&
				      v34_data_quality_is_valid(&quality),
			      "recovery budget did not rearm at the clean-window boundary: %u", window);
		}
		for (unsigned attempt = 0; attempt <= V34_DATA_QUALITY_MAX_RETRAINS; ++attempt) {
			measurement.decision_metric = 40;
			CHECK(v34_data_quality_start(&quality, V34_SYMBOL_RATE_3429, 12,
				      &measurement, 1) == V34_DATA_QUALITY_OK,
			      "rearmed quality did not restart");
			for (unsigned window = 0; window < quality.loss_windows; ++window) {
				enum v34_data_quality_result expected = window == 0 ?
					V34_DATA_QUALITY_FREEZE_TIMING :
					(window + 1 == quality.loss_windows &&
					 attempt < V34_DATA_QUALITY_MAX_RETRAINS ?
					 V34_DATA_QUALITY_RETRAIN : V34_DATA_QUALITY_OK);

				CHECK(publish_quality_window(&quality, &measurement, 200) == expected,
				      "rearmed quality bypassed the consecutive-failure bound");
			}
		}
	}
	for (unsigned interruption = 0; interruption < 4; ++interruption) {
		struct v34_adaptive_equalizer_quality measurement = { .decision_metric = 40 };
		struct v34_data_quality quality = { .recovery.attempts = 2 };

		CHECK(v34_data_quality_start(&quality, V34_SYMBOL_RATE_3429, 12,
			      &measurement, 1) == V34_DATA_QUALITY_OK, "interruption fixture did not start");
		for (unsigned window = 1; window < V34_DATA_QUALITY_REARM_WINDOWS; ++window)
			CHECK(publish_quality_window(&quality, &measurement, 40) == V34_DATA_QUALITY_OK,
			      "clean window failed before interruption");
		if (interruption < 2) {
			/* Frozen timing below the retrain threshold is not healthy evidence. */
			CHECK(publish_quality_window(&quality, &measurement, interruption ? 200 : 91) ==
				      V34_DATA_QUALITY_FREEZE_TIMING && quality.recovery.attempts == 2,
			      "uncertain or bad quality rearmed recovery");
		} else {
			measurement.decision_metric = interruption == 3 ? 5000 : 40;
			CHECK(v34_data_quality_start(&quality,
				      interruption == 3 ? V34_SYMBOL_RATE_2400 : V34_SYMBOL_RATE_3429,
				      interruption == 3 ? 1 : 12, &measurement, 1) == V34_DATA_QUALITY_OK,
			      "quality restart failed");
		}
		for (unsigned window = 1; window <= V34_DATA_QUALITY_REARM_WINDOWS; ++window) {
			CHECK(publish_quality_window(&quality, &measurement, 40) ==
				      (window == 1 && interruption < 2 ?
				       V34_DATA_QUALITY_RESUME_TIMING : V34_DATA_QUALITY_OK) &&
				      quality.recovery.attempts ==
				      (window == V34_DATA_QUALITY_REARM_WINDOWS && interruption != 3 ? 0 : 2),
			      "interruption %u did not clear clean credit at window %u", interruption, window);
		}
	}
	return true;
}

static bool test_data_quality_transactionality(void)
{
	struct v34_adaptive_equalizer_quality measurement = {
		.window_position = 1,
	};
	struct v34_data_quality quality;
	struct v34_data_quality saved;
	uint8_t selected_rate = 7;

	CHECK(v34_data_quality_init(NULL) ==
		      V34_DATA_QUALITY_INVALID_ARGUMENT &&
	      v34_data_quality_init(&quality) == V34_DATA_QUALITY_OK &&
	      v34_data_quality_is_valid(&quality),
	      "quality owner did not initialize transactionally");
	saved = quality;
	for (unsigned stride = 0; stride <= 3; stride += 3) {
		CHECK(v34_data_quality_start(
			      &quality, V34_SYMBOL_RATE_3429, 14,
			      &measurement, stride) == V34_DATA_QUALITY_INVALID_ARGUMENT &&
		      memcmp(&quality, &saved, sizeof(quality)) == 0,
		      "invalid measurement cadence changed the quality owner");
	}
	CHECK(v34_data_quality_observe(&quality, &measurement) ==
		      V34_DATA_QUALITY_INVALID_STATE &&
	      v34_data_quality_start(
		      &quality, V34_SYMBOL_RATE_COUNT, 14, &measurement, 1) ==
		      V34_DATA_QUALITY_INVALID_ARGUMENT &&
	      v34_data_quality_start(
		      &quality, V34_SYMBOL_RATE_2400, 10, &measurement, 1) ==
		      V34_DATA_QUALITY_INVALID_ARGUMENT &&
	      v34_data_quality_start(
		      &quality, V34_SYMBOL_RATE_3429, 14,
		      (const struct v34_adaptive_equalizer_quality *)&quality, 1) ==
		      V34_DATA_QUALITY_INVALID_ARGUMENT &&
	      memcmp(&quality, &saved, sizeof(quality)) == 0,
	      "quality setup rejection changed the owner");
	CHECK(v34_data_quality_select_receive_rate(
		      V34_SYMBOL_RATE_COUNT, 0x3fffu, 14, &measurement,
		      &selected_rate) == V34_DATA_QUALITY_INVALID_ARGUMENT &&
	      v34_data_quality_select_receive_rate(
		      V34_SYMBOL_RATE_3429, 0xc000u, 14, &measurement,
		      &selected_rate) == V34_DATA_QUALITY_INVALID_ARGUMENT &&
	      v34_data_quality_select_receive_rate(
		      V34_SYMBOL_RATE_3429, 0x3fffu, 0, &measurement,
		      &selected_rate) == V34_DATA_QUALITY_INVALID_ARGUMENT &&
	      v34_data_quality_select_receive_rate(
		      V34_SYMBOL_RATE_3429, 0x3fffu, 14, &measurement,
		      (uint8_t *)&measurement) ==
		      V34_DATA_QUALITY_INVALID_ARGUMENT &&
	      selected_rate == 7,
	      "rate-selection rejection changed its output");
	CHECK(v34_data_quality_start(
		      &quality, V34_SYMBOL_RATE_3429, 14, &measurement, 1) ==
		      V34_DATA_QUALITY_OK,
	      "quality transaction fixture did not start");
	saved = quality;
	for (unsigned invalid = 0; invalid < 3; ++invalid) {
		quality = saved;
		quality.loss_windows = invalid == 0 ? 0 : invalid == 1 ? 2 : 8;
		CHECK(v34_data_quality_observe(&quality, &measurement) ==
			      V34_DATA_QUALITY_INVALID_STATE,
		      "invalid recovery duration accepted");
	}
	quality = saved;
	measurement.decision_metric = -1;
	CHECK(v34_data_quality_observe(&quality, &measurement) ==
		      V34_DATA_QUALITY_INVALID_ARGUMENT &&
	      memcmp(&quality, &saved, sizeof(quality)) == 0,
	      "invalid measurement changed the quality owner");
	quality.recovery.attempts = V34_DATA_QUALITY_MAX_RETRAINS + 1u;
	CHECK(!v34_data_quality_is_valid(&quality),
	      "invalid quality retrain count was accepted");
	quality = saved;
	quality.window_position = V34_ADAPTIVE_EQUALIZER_QUALITY_SYMBOLS;
	CHECK(!v34_data_quality_is_valid(&quality),
	      "invalid completed-window position was accepted");
	for (unsigned invalid = 0; invalid < 6; ++invalid) {
		quality = saved;
		quality.recovery.attempts = 1;
		quality.recovery.good_windows = 1;
		switch (invalid) {
		case 0: quality.recovery.good_windows = V34_DATA_QUALITY_REARM_WINDOWS; break;
		case 1: quality.recovery.attempts = 0; break;
		case 2: quality.recovery.bad_windows = 1; break;
		case 3: quality.threshold = 0; break;
		case 4: quality.timing_frozen = true; break;
		case 5: quality.active = false; break;
		}
		CHECK(!v34_data_quality_is_valid(&quality),
		      "invalid healthy-credit state %u accepted", invalid);
	}
	return true;
}

int main(void)
{
	if (!test_training_decoder_power() ||
	    !test_initial_state_and_carrier_profile() ||
	    !test_carrier_phase_accumulation() ||
	    !test_carrier_coast() ||
	    !test_carrier_only_commit() ||
	    !test_retained_pair_geometry(V34_EQUALIZER_TAPS, 20u) ||
	    !test_retained_pair_geometry(V34_EQUALIZER_MAX_TAPS, 16u) ||
	    !test_data_carrier_tracking() ||
	    !test_exact_decision_feedback_oracle() ||
	    !test_feedback_history_lineage() ||
	    !test_feedback_full_scale_projection() ||
	    !test_feedback_and_discard_lifecycle() ||
	    !test_discard_lineage_zero() ||
	    !test_feedback_training_boundary() ||
	    !test_tracking_boundary() || !test_decision_quality_window() ||
	    !test_transactionality() || !test_data_quality_policy() ||
	    !test_data_quality_duration() ||
	    !test_recent_rate_measurement() ||
	    !test_paired_data_quality_windows() || !test_data_quality_rearm() ||
	    !test_data_quality_transactionality())
		return 1;
	return 0;
}
