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

	{
		struct v34_adaptive_equalizer equalizer;
		struct v34_adaptive_equalizer_observation observation;
		struct v34_adaptive_equalizer_decision decision;

		CHECK(v34_adaptive_equalizer_init(&equalizer) ==
			      V34_ADAPTIVE_EQUALIZER_OK,
		      "signed carrier phase owner did not initialize");
		equalizer.carrier_phase_fraction = 1;
		equalizer.carrier_phase_low = UINT16_MAX;
		CHECK(push_zero_symbol(&equalizer),
		      "signed carrier phase input failed");
		CHECK(v34_adaptive_equalizer_observe(
			      &equalizer, &observation) ==
			      V34_ADAPTIVE_EQUALIZER_OK,
		      "signed carrier phase observation failed");
		decision = zero_decision(&observation);
		CHECK(v34_adaptive_equalizer_commit(&equalizer, &decision) ==
			      V34_ADAPTIVE_EQUALIZER_OK &&
		      equalizer.carrier_phase_fraction == 0 &&
		      equalizer.carrier_phase_low == UINT16_MAX &&
		      equalizer.rotate_in_phase == 0x4000 &&
		      equalizer.rotate_quadrature == 0,
		      "carrier low word lost its signed lineage");
	}
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
	CHECK(observation_hash == UINT64_C(0xf9bec92bd51138ed),
	      "observation oracle changed: %#llx",
	      (unsigned long long)observation_hash);
	CHECK(state_hash(&equalizer) == UINT64_C(0x72ec1bba8e50fbce),
	      "terminal oracle changed: %#llx",
	      (unsigned long long)state_hash(&equalizer));
	CHECK(equalizer.phase_accumulator == -8971410 &&
	      equalizer.phase_error == -324130468 &&
	      equalizer.rotate_in_phase == 14508 &&
	      equalizer.rotate_quadrature == -7613 &&
	      equalizer.carrier_phase_low == 29358 &&
	      equalizer.carrier_phase_fraction == 7562 &&
	      equalizer.equalizer.index == 32 && equalizer.epoch == 96 &&
	      equalizer.lineage == UINT64_C(0xd3e3fb96b7dabaf6) &&
	      !equalizer.decision_pending &&
	      v34_adaptive_equalizer_is_valid(&equalizer),
	      "terminal oracle scalar state changed");
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
	      equalizer.quality.window_position == 0,
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
	}
	CHECK(equalizer.quality.decision_metric == 15625 &&
	      equalizer.quality.feedback_metric == 15 &&
	      equalizer.quality.reference_metric == UINT32_C(15624) &&
	      equalizer.quality.window_position == 0 &&
	      equalizer.quality.decision_accumulator == 0 &&
	      equalizer.quality.feedback_accumulator == 0 &&
	      equalizer.quality.reference_accumulator == 0,
	      "published quality window changed");
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
	      memcmp(&observation, &saved_observation,
		     sizeof(observation)) == 0,
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
		{ V34_SYMBOL_RATE_2400, 9, 135 },
		{ V34_SYMBOL_RATE_2743, 10, 157 },
		{ V34_SYMBOL_RATE_2800, 11, 84 },
		{ V34_SYMBOL_RATE_3000, 12, 74 },
		{ V34_SYMBOL_RATE_3200, 13, 76 },
		{ V34_SYMBOL_RATE_3429, 14, 67 },
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
			      profiles[index].rate_units, &measurement) ==
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
				      &measurement) == V34_DATA_QUALITY_OK,
			      "quality profile rejected %zu/%u", index, rate);
			profile_hash = hash_word(
				profile_hash, (uint16_t)quality.threshold);
		}
	}
	CHECK(profile_hash == UINT64_C(0xd16019b57923c441),
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
		      &quality, V34_SYMBOL_RATE_2400, 1, &measurement) ==
		      V34_DATA_QUALITY_OK &&
	      quality.threshold == 800,
	      "minimum-rate training baseline changed: %d", quality.threshold);
	measurement.decision_metric = 5000;
	CHECK(v34_data_quality_start(
		      &quality, V34_SYMBOL_RATE_2743, 2, &measurement) ==
		      V34_DATA_QUALITY_OK &&
	      quality.threshold == 0 && quality.active,
	      "saturated minimum-rate baseline was not disabled");
	CHECK(publish_quality_window(&quality, &measurement, INT16_MAX) ==
		      V34_DATA_QUALITY_OK &&
	      quality.bad_windows == 0,
	      "disabled minimum-rate quality requested recovery");
	measurement.decision_metric = 0;
	measurement.window_position = 1;

	CHECK(v34_data_quality_init(&quality) == V34_DATA_QUALITY_OK &&
	      v34_data_quality_start(
		      &quality, V34_SYMBOL_RATE_3429, 14, &measurement) ==
		      V34_DATA_QUALITY_OK &&
	      quality.threshold == 67,
	      "retained quality profile did not initialize");
	measurement.decision_metric = 192;
	measurement.window_position = 0;
	CHECK(v34_data_quality_observe(&quality, &measurement) ==
		      V34_DATA_QUALITY_OK &&
	      quality.bad_windows == 1 && quality.window_latched &&
	      v34_data_quality_observe(&quality, &measurement) ==
		      V34_DATA_QUALITY_OK &&
	      quality.bad_windows == 1,
	      "one bad window was not consumed exactly once");
	CHECK(publish_quality_window(&quality, &measurement, 66) ==
		      V34_DATA_QUALITY_OK &&
	      quality.bad_windows == 0,
	      "a recovered window did not clear quality loss");
	for (unsigned int window = 0;
	     window < V34_DATA_QUALITY_LOSS_WINDOWS; ++window) {
		enum v34_data_quality_result expected =
			window + 1u == V34_DATA_QUALITY_LOSS_WINDOWS ?
				V34_DATA_QUALITY_RETRAIN :
				V34_DATA_QUALITY_OK;

		CHECK(publish_quality_window(&quality, &measurement, 192) ==
			      expected,
		      "quality loss result changed at window %u", window);
	}
	CHECK(!quality.active && quality.automatic_retrains == 1 &&
	      v34_data_quality_is_valid(&quality),
	      "first quality retrain did not preserve a valid budget");

	for (unsigned int retry = 1;
	     retry < V34_DATA_QUALITY_MAX_RETRAINS; ++retry) {
		measurement.window_position = 0;
		CHECK(v34_data_quality_start(
			      &quality, V34_SYMBOL_RATE_3429, 14,
			      &measurement) == V34_DATA_QUALITY_OK,
		      "quality generation %u did not restart", retry);
		for (unsigned int window = 0;
		     window < V34_DATA_QUALITY_LOSS_WINDOWS; ++window) {
			enum v34_data_quality_result expected =
				window + 1u == V34_DATA_QUALITY_LOSS_WINDOWS ?
					V34_DATA_QUALITY_RETRAIN :
					V34_DATA_QUALITY_OK;

			CHECK(publish_quality_window(
				      &quality, &measurement, 198) == expected,
			      "quality retry %u window %u changed", retry,
			      window);
		}
	}
	CHECK(quality.automatic_retrains == V34_DATA_QUALITY_MAX_RETRAINS &&
	      !quality.active,
	      "quality retrain budget did not saturate");
	measurement.window_position = 1;
	CHECK(v34_data_quality_start(
		      &quality, V34_SYMBOL_RATE_3200, 11, &measurement) ==
		      V34_DATA_QUALITY_OK &&
	      quality.threshold == 209,
	      "26400 quality profile changed");
	for (unsigned int window = 0;
	     window < V34_DATA_QUALITY_LOSS_WINDOWS; ++window)
		CHECK(publish_quality_window(&quality, &measurement, 345) ==
			      V34_DATA_QUALITY_OK,
		      "exhausted quality budget retriggered at %u", window);
	CHECK(quality.active &&
	      quality.bad_windows == V34_DATA_QUALITY_LOSS_WINDOWS &&
	      publish_quality_window(&quality, &measurement, 166) ==
		      V34_DATA_QUALITY_OK &&
	      quality.bad_windows == 0,
	      "exhausted quality monitor did not retain recovery evidence");
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
	CHECK(v34_data_quality_observe(&quality, &measurement) ==
		      V34_DATA_QUALITY_INVALID_STATE &&
	      v34_data_quality_start(
		      &quality, V34_SYMBOL_RATE_COUNT, 14, &measurement) ==
		      V34_DATA_QUALITY_INVALID_ARGUMENT &&
	      v34_data_quality_start(
		      &quality, V34_SYMBOL_RATE_2400, 10, &measurement) ==
		      V34_DATA_QUALITY_INVALID_ARGUMENT &&
	      v34_data_quality_start(
		      &quality, V34_SYMBOL_RATE_3429, 14,
		      (const struct v34_adaptive_equalizer_quality *)&quality) ==
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
		      &quality, V34_SYMBOL_RATE_3429, 14, &measurement) ==
		      V34_DATA_QUALITY_OK,
	      "quality transaction fixture did not start");
	saved = quality;
	measurement.decision_metric = -1;
	CHECK(v34_data_quality_observe(&quality, &measurement) ==
		      V34_DATA_QUALITY_INVALID_ARGUMENT &&
	      memcmp(&quality, &saved, sizeof(quality)) == 0,
	      "invalid measurement changed the quality owner");
	quality.automatic_retrains = V34_DATA_QUALITY_MAX_RETRAINS + 1u;
	CHECK(!v34_data_quality_is_valid(&quality),
	      "invalid quality retrain count was accepted");
	return true;
}

int main(void)
{
	if (!test_initial_state_and_carrier_profile() ||
	    !test_exact_decision_feedback_oracle() ||
	    !test_feedback_and_discard_lifecycle() ||
	    !test_feedback_training_boundary() ||
	    !test_tracking_boundary() || !test_decision_quality_window() ||
	    !test_transactionality() || !test_data_quality_policy() ||
	    !test_data_quality_transactionality())
		return 1;
	return 0;
}
