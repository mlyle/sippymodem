// SPDX-License-Identifier: BSD-3-Clause

#include <v90_phase3_ja_rx.h>
#include <v90_phase3_analog_tx.h>

#include "test.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static struct qam_point multiply(
	const struct qam_point *left, const struct qam_point *right)
{
	return (struct qam_point){
		.in_phase = left->in_phase * right->in_phase -
			    left->quadrature * right->quadrature,
		.quadrature = left->in_phase * right->quadrature +
			      left->quadrature * right->in_phase,
	};
}

static struct qam_point rotate(
	struct qam_point point, unsigned int quarter_turns)
{
	switch (quarter_turns & 3u) {
	case 0:
		return point;
	case 1:
		return (struct qam_point){ -point.quadrature, point.in_phase };
	case 2:
		return (struct qam_point){ -point.in_phase, -point.quadrature };
	case 3:
		return (struct qam_point){ point.quadrature, -point.in_phase };
	}
	return (struct qam_point){ 0 };
}

static double noise_component(uint32_t *state)
{
	*state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
	return ((double)((*state >> 8u) & UINT32_C(0xffff)) /
		UINT16_MAX - 0.5) * 0.16;
}

static bool test_precoder_training(void)
{
	static const struct qam_point expected[V34_PRECODER_TAP_COUNT] = {
		{ 0.31, 0.09 },
		{ -0.12, 0.055 },
		{ 0.045, -0.025 },
	};
	struct v34_precoder_trainer trainer;
	struct v34_precoder_recommendation recommendation;
	struct qam_point history[V34_PRECODER_TAP_COUNT] = { 0 };
	uint32_t noise_state = UINT32_C(0x13579bdf);

	CHECK(v34_precoder_trainer_init(&trainer) ==
		      V34_PRECODER_TRAINER_OK,
	      "precoder trainer did not initialize");
	for (size_t symbol = 0; symbol < 60000; ++symbol) {
		struct qam_point target =
			v34_rotated_point_zero((int)(symbol & 3u));
		struct qam_point error = {
			.in_phase = noise_component(&noise_state),
			.quadrature = noise_component(&noise_state),
		};
		struct qam_point decision;

		for (size_t tap = 0; tap < V34_PRECODER_TAP_COUNT; ++tap) {
			struct qam_point prediction = multiply(
				&expected[tap], &history[tap]);

			error.in_phase -= prediction.in_phase;
			error.quadrature -= prediction.quadrature;
		}
		decision = (struct qam_point){
			.in_phase = target.in_phase + error.in_phase,
			.quadrature = target.quadrature + error.quadrature,
		};
		CHECK(v34_precoder_trainer_observe(
			      &trainer, &decision, &target) ==
			      V34_PRECODER_TRAINER_OK,
		      "precoder trainer stopped at symbol %zu", symbol);
		for (size_t tap = V34_PRECODER_TAP_COUNT - 1u;
		     tap != 0; --tap)
			history[tap] = history[tap - 1u];
		history[0] = error;
	}
	CHECK(v34_precoder_trainer_finish(&trainer, &recommendation) ==
		      V34_PRECODER_TRAINER_OK &&
	      recommendation.enabled,
	      "correlated decision errors did not select a precoder");
	for (size_t tap = 0; tap < V34_PRECODER_TAP_COUNT; ++tap) {
		int expected_real = (int)lround(
			expected[tap].in_phase * V34_Q14_SCALE);
		int expected_imaginary = (int)lround(
			expected[tap].quadrature * V34_Q14_SCALE);

		CHECK(abs((int)recommendation.coefficients[tap].real -
			  expected_real) < 900 &&
		      abs((int)recommendation.coefficients[tap].imaginary -
			  expected_imaginary) < 900,
		      "precoder tap %zu did not converge: %d/%d expected %d/%d",
		      tap, recommendation.coefficients[tap].real,
		      recommendation.coefficients[tap].imaginary,
		      expected_real, expected_imaginary);
	}
	CHECK(v34_precoder_trainer_init(&trainer) ==
		      V34_PRECODER_TRAINER_OK,
	      "flat-channel trainer did not initialize");
	for (size_t symbol = 0; symbol < 600; ++symbol) {
		struct qam_point target =
			v34_rotated_point_zero((int)(symbol & 3u));

		CHECK(v34_precoder_trainer_observe(
			      &trainer, &target, &target) ==
			      V34_PRECODER_TRAINER_OK,
		      "flat-channel trainer stopped at symbol %zu", symbol);
	}
	CHECK(v34_precoder_trainer_finish(&trainer, &recommendation) ==
		      V34_PRECODER_TRAINER_OK &&
	      !recommendation.enabled &&
	      v34_precoder_recommendation_is_valid(&recommendation),
	      "flat decision errors selected a precoder");
	return true;
}

static bool run_timeline(
	uint32_t trn_symbols, unsigned int quarter_turns)
{
	struct v90_phase3_analog_tx tx;
	struct v90_phase3_ja_rx rx;
	struct v90_phase3_ja_handoff handoff;
	bool started_ja = false;

	CHECK(v90_phase3_analog_tx_init(&tx) == V90_PHASE3_ANALOG_TX_OK &&
	      v90_phase3_ja_rx_init(&rx) == V90_PHASE3_JA_RX_OK,
	      "TRN %u phase %u timeline did not initialize",
	      trn_symbols, quarter_turns);
	for (size_t emitted = 0; emitted < 5000; ++emitted) {
		struct qam_point point;
		bool post_pp = tx.stage == V90_PHASE3_ANALOG_TX_JA ||
			tx.sequence.prefix.stage == V34_PHASE3_TRANSMITTER_TRN;
		enum v90_phase3_ja_rx_result result;

		if (!started_ja && post_pp &&
		    tx.sequence.prefix.trn_symbols == trn_symbols) {
			CHECK(v90_phase3_analog_tx_start_ja(&tx) ==
				      V90_PHASE3_ANALOG_TX_OK,
			      "TRN %u phase %u could not start Ja",
			      trn_symbols, quarter_turns);
			started_ja = true;
		}
		CHECK(v90_phase3_analog_tx_next(&tx, &point) ==
			      V90_PHASE3_ANALOG_TX_OK,
		      "TRN %u phase %u source stopped at %zu",
		      trn_symbols, quarter_turns, emitted);
		/* S/PP acquisition belongs to the fixed line owner. The semantic
		 * owner starts at the first post-PP TRN decision, without delay. */
		if (!post_pp)
			continue;
		point = rotate(point, quarter_turns);
		result = v90_phase3_ja_rx_observe(&rx, &point);
		if (result == V90_PHASE3_JA_RX_DETECTED)
			break;
		CHECK(result == V90_PHASE3_JA_RX_OK,
		      "TRN %u phase %u receiver returned %d at %zu",
		      trn_symbols, quarter_turns, result, emitted);
	}
	CHECK(started_ja && v90_phase3_ja_rx_is_complete(&rx) &&
	      rx.precoder.observed_symbols > V34_TRAINING_SEQUENCE_MIN_SYMBOLS &&
	      v90_phase3_ja_rx_handoff(&rx, &handoff) == V90_PHASE3_JA_RX_OK &&
	      handoff.dil.segment_count == 0u &&
	      v34_precoder_recommendation_is_valid(&handoff.precoder),
	      "TRN %u phase %u did not recover the complete Ja boundary",
	      trn_symbols, quarter_turns);
	{
		struct v90_phase3_ja_rx saved = rx;
		struct qam_point point = { 1.0, 1.0 };
		struct qam_point saved_point = point;

		CHECK(v90_phase3_ja_rx_observe(&rx, &point) ==
			      V90_PHASE3_JA_RX_COMPLETE &&
		      memcmp(&rx, &saved, sizeof(rx)) == 0 &&
		      memcmp(&point, &saved_point, sizeof(point)) == 0,
		      "TRN %u phase %u complete receiver consumed a point",
		      trn_symbols, quarter_turns);
	}
	return true;
}

static bool test_generated_timelines(void)
{
	CHECK(run_timeline(V34_TRAINING_SEQUENCE_MIN_SYMBOLS, 0) &&
	      run_timeline(V34_TRAINING_SEQUENCE_MIN_SYMBOLS + 137u, 1) &&
	      run_timeline(V34_TRAINING_SEQUENCE_MIN_SYMBOLS + 251u, 3),
	      "generated Ja receive timelines failed");
	return true;
}

static bool test_guard_and_transactionality(void)
{
	struct v90_phase3_ja_rx rx;
	struct v90_phase3_ja_rx saved;
	struct v90_phase3_ja_rx corrupt;
	struct v90_phase3_ja_handoff handoff;
	struct v90_phase3_ja_handoff saved_handoff;
	struct qam_point decision = v34_rotated_point_zero(0);
	struct qam_point saved_decision = decision;
	struct qam_point invalid = { NAN, 0.0 };

	CHECK(v90_phase3_ja_rx_init(NULL) == V90_PHASE3_JA_RX_INVALID_ARGUMENT &&
	      v90_phase3_ja_rx_init(&rx) == V90_PHASE3_JA_RX_OK &&
	      v90_phase3_ja_rx_is_valid(&rx) &&
	      !v90_phase3_ja_rx_is_complete(&rx),
	      "Ja semantic owner did not initialize");
	saved = rx;
	memset(&handoff, 0xa5, sizeof(handoff));
	saved_handoff = handoff;
	CHECK(v90_phase3_ja_rx_observe(NULL, &decision) ==
		      V90_PHASE3_JA_RX_INVALID_ARGUMENT &&
	      v90_phase3_ja_rx_observe(&rx, &invalid) ==
		      V90_PHASE3_JA_RX_INVALID_ARGUMENT &&
	      v90_phase3_ja_rx_observe(&rx, NULL) ==
		      V90_PHASE3_JA_RX_INVALID_ARGUMENT &&
	      v90_phase3_ja_rx_observe(&rx, &rx.precoder.error_history[0]) ==
		      V90_PHASE3_JA_RX_INVALID_ARGUMENT &&
	      v90_phase3_ja_rx_handoff(&rx, &handoff) ==
		      V90_PHASE3_JA_RX_INVALID_STATE &&
	      v90_phase3_ja_rx_handoff(&rx, NULL) ==
		      V90_PHASE3_JA_RX_INVALID_ARGUMENT &&
	      v90_phase3_ja_rx_handoff(
		      &rx, (struct v90_phase3_ja_handoff *)(void *)&rx) ==
		      V90_PHASE3_JA_RX_INVALID_ARGUMENT &&
	      memcmp(&rx, &saved, sizeof(rx)) == 0 &&
	      memcmp(&decision, &saved_decision, sizeof(decision)) == 0 &&
	      memcmp(&handoff, &saved_handoff, sizeof(handoff)) == 0,
	      "rejected Ja call changed its receiver or destination");
	invalid = (struct qam_point){ DBL_MAX, DBL_MAX };
	CHECK(v90_phase3_ja_rx_observe(&rx, &invalid) ==
		      V90_PHASE3_JA_RX_NUMERIC_FAILURE &&
	      memcmp(&rx, &saved, sizeof(rx)) == 0,
	      "numeric Ja failure changed its receiver");
	corrupt = rx;
	corrupt.search.has_previous_rotation = true;
	CHECK(!v90_phase3_ja_rx_is_valid(NULL) &&
	      !v90_phase3_ja_rx_is_complete(NULL) &&
	      !v90_phase3_ja_rx_is_valid(&corrupt) &&
	      v90_phase3_ja_rx_observe(&corrupt, &decision) ==
		      V90_PHASE3_JA_RX_INVALID_ARGUMENT,
	      "invalid pre-guard Ja search was accepted");
	for (unsigned int index = 0;
	     index < V34_TRAINING_SEQUENCE_MIN_SYMBOLS; ++index) {
		CHECK(v90_phase3_ja_rx_observe(&rx, &decision) ==
			      V90_PHASE3_JA_RX_OK &&
		      rx.precoder.observed_symbols == index + 1u &&
		      memcmp(&rx.search, &saved.search, sizeof(rx.search)) == 0,
		      "Ja search consumed required TRN decision %u", index);
	}
	CHECK(v90_phase3_ja_rx_observe(&rx, &decision) == V90_PHASE3_JA_RX_OK &&
	      rx.search.has_previous_rotation && rx.search.warmup_bits == 0u,
	      "first decision after 512 TRN symbols did not seed Ja search");
	rx.precoder.observed_symbols = UINT32_MAX;
	rx.precoder.measured_symbols = UINT32_MAX -
		V34_PRECODER_TRAINER_SETTLING_SYMBOLS + 1u;
	saved = rx;
	CHECK(v90_phase3_ja_rx_is_valid(&rx) &&
	      v90_phase3_ja_rx_observe(&rx, &decision) ==
		      V90_PHASE3_JA_RX_OUT_OF_RANGE &&
	      memcmp(&rx, &saved, sizeof(rx)) == 0 &&
	      memcmp(&decision, &saved_decision, sizeof(decision)) == 0,
	      "exhausted Ja owner wrapped its symbol count");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "generated Ja timelines", test_generated_timelines },
		{ "precoder training", test_precoder_training },
		{ "TRN guard and transactionality", test_guard_and_transactionality },
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
