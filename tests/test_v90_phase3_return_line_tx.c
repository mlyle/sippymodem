// SPDX-License-Identifier: BSD-3-Clause

#include <v90_phase3_return_line_tx.h>

#include <v90_phase3.h>

#include "test.h"

#include <string.h>

enum {
	NOMINAL_AMPLITUDE = 6000,
	SAMPLE_LIMIT = 10000,
};

static struct v34_phase3_profile profile(void)
{
	return (struct v34_phase3_profile){
		.symbol_rate_numerator = 3000,
		.symbol_rate_denominator = 1,
		.carrier_frequency_numerator = 2000,
		.carrier_frequency_denominator = 1,
		.symbol_rate = V34_SYMBOL_RATE_3000,
		.local_role = V34_ANSWERING_STATION,
		.preemphasis_filter_index = 0,
		.projected_data_rate_2400_bps_units = 8,
		.high_carrier = true,
	};
}

static bool model_sample(
	struct v34_training_line *line,
	struct v90_phase3_return_tx *points,
	struct v34_training_sequence *idle_scr, int16_t *sample)
{
	struct qam_point point;
	const struct qam_point *supplied = NULL;
	bool accepting = v34_training_line_needs_point(line);
	bool accepted;

	if (accepting) {
		enum v90_phase3_return_tx_stage point_stage = points->stage;
		uint8_t quarter_point;
		uint8_t rotation;

		if (v90_phase3_return_tx_next(points, &point) !=
		    V90_PHASE3_RETURN_TX_OK)
			return false;
		if (point_stage == V90_PHASE3_RETURN_TX_SILENCE &&
		    idle_scr != NULL &&
		    v34_training_sequence_next(
			    idle_scr, &point, &quarter_point, &rotation) !=
			    V34_TRAINING_SEQUENCE_OK)
			return false;
		supplied = &point;
	}
	return v34_training_line_sample(
		       line, supplied, sample, &accepted) ==
		       V34_TRAINING_LINE_OK &&
	       accepted == accepting;
}

static bool test_exact_continuation(void)
{
	struct v34_phase3_profile selected = profile();
	struct v34_training_line source;
	struct v34_training_line model_line;
	struct v34_training_line handoff;
	struct v90_phase3_return_line_tx tx;
	struct v90_phase3_return_tx model_points;
	bool started = false;
	bool finished = false;
	bool complete = false;

	CHECK(v34_training_line_init(
		      &source, &selected, V90_PHASE3_SAMPLE_RATE,
		      NOMINAL_AMPLITUDE) == V34_TRAINING_LINE_OK &&
	      v90_phase3_return_line_tx_init(&tx, &source) ==
		      V90_PHASE3_RETURN_LINE_TX_OK &&
	      v90_phase3_return_tx_init(&model_points) ==
		      V90_PHASE3_RETURN_TX_OK,
	      "return line did not initialize at a point boundary");
	model_line = source;
	for (uint32_t sample_index = 0;
	     sample_index < SAMPLE_LIMIT; ++sample_index) {
		int16_t actual;
		int16_t expected;

		if (!started && model_points.silence_symbols == 41) {
			CHECK(v90_phase3_return_line_tx_start_s(&tx) ==
				      V90_PHASE3_RETURN_LINE_TX_OK &&
			      v90_phase3_return_tx_start_s(&model_points) ==
				      V90_PHASE3_RETURN_TX_OK,
			      "return line could not start S");
			started = true;
		}
		if (started && !finished && model_points.s_symbols == 73) {
			CHECK(v90_phase3_return_line_tx_finish_s(&tx) ==
				      V90_PHASE3_RETURN_LINE_TX_OK &&
			      v90_phase3_return_tx_finish_s(&model_points) ==
				      V90_PHASE3_RETURN_TX_OK,
			      "return line could not finish S");
			finished = true;
		}
		if (v90_phase3_return_line_tx_is_handoff_ready(&tx)) {
			complete = true;
			break;
		}
		CHECK(model_sample(
			      &model_line, &model_points, NULL, &expected) &&
		      v90_phase3_return_line_tx_sample(&tx, &actual) ==
			      V90_PHASE3_RETURN_LINE_TX_OK &&
		      actual == expected &&
		      memcmp(&tx.line, &model_line, sizeof(model_line)) == 0 &&
		      memcmp(&tx.points, &model_points,
			     sizeof(model_points)) == 0,
		      "return waveform diverged at sample %u", sample_index);
	}
	CHECK(started && finished && complete &&
	      v90_phase3_return_line_tx_handoff(&tx, &handoff) ==
		      V90_PHASE3_RETURN_LINE_TX_OK &&
	      memcmp(&handoff, &model_line, sizeof(handoff)) == 0,
	      "return line did not preserve its exact handoff");
	{
		struct v90_phase3_return_line_tx saved = tx;
		int16_t sample = 1234;

		CHECK(v90_phase3_return_line_tx_sample(&tx, &sample) ==
			      V90_PHASE3_RETURN_LINE_TX_COMPLETE &&
		      memcmp(&tx, &saved, sizeof(tx)) == 0 && sample == 1234,
		      "complete return line consumed another sample");
	}
	return true;
}

static bool test_scr_idle_continuation(void)
{
	struct v34_phase3_profile selected = profile();
	struct v34_training_line source;
	struct v34_training_line model_line;
	struct v90_phase3_return_line_tx tx;
	struct v90_phase3_return_tx model_points;
	struct v34_training_sequence model_scr;
	uint64_t absolute_sum = 0;
	bool started = false;
	bool finished = false;

	CHECK(v34_training_line_init(
		      &source, &selected, V90_PHASE3_SAMPLE_RATE,
		      NOMINAL_AMPLITUDE) == V34_TRAINING_LINE_OK &&
	      v90_phase3_return_line_tx_init_scr(
		      &tx, &source, V34_CONSTELLATION_16_POINT) ==
		      V90_PHASE3_RETURN_LINE_TX_OK &&
	      v90_phase3_return_tx_init(&model_points) ==
		      V90_PHASE3_RETURN_TX_OK &&
	      v34_training_sequence_init(
		      &model_scr, V34_ANSWERING_STATION,
		      V34_CONSTELLATION_16_POINT) == V34_TRAINING_SEQUENCE_OK,
	      "SCR return line did not initialize");
	model_line = source;
	for (uint32_t sample_index = 0;
	     sample_index < SAMPLE_LIMIT; ++sample_index) {
		int16_t actual;
		int16_t expected;

		if (!started && model_points.silence_symbols == 96) {
			CHECK(v90_phase3_return_line_tx_start_s(&tx) ==
				      V90_PHASE3_RETURN_LINE_TX_OK &&
			      v90_phase3_return_tx_start_s(&model_points) ==
				      V90_PHASE3_RETURN_TX_OK,
			      "SCR return line could not start S");
			started = true;
		}
		if (started && !finished && model_points.s_symbols == 128) {
			CHECK(v90_phase3_return_line_tx_finish_s(&tx) ==
				      V90_PHASE3_RETURN_LINE_TX_OK &&
			      v90_phase3_return_tx_finish_s(&model_points) ==
				      V90_PHASE3_RETURN_TX_OK,
			      "SCR return line could not finish S");
			finished = true;
		}
		if (v90_phase3_return_line_tx_is_handoff_ready(&tx))
			break;
		CHECK(model_sample(
			      &model_line, &model_points, &model_scr,
			      &expected) &&
		      v90_phase3_return_line_tx_sample(&tx, &actual) ==
			      V90_PHASE3_RETURN_LINE_TX_OK &&
		      actual == expected &&
		      memcmp(&tx.line, &model_line, sizeof(model_line)) == 0 &&
		      memcmp(&tx.points, &model_points,
			     sizeof(model_points)) == 0 &&
		      memcmp(&tx.idle_scr, &model_scr,
			     sizeof(model_scr)) == 0,
		      "SCR return waveform diverged at sample %u", sample_index);
		if (!started)
			absolute_sum += actual < 0 ?
					(uint64_t)(-(int32_t)actual) :
					(uint64_t)actual;
	}
	CHECK(started && finished && absolute_sum != 0u &&
	      v90_phase3_return_line_tx_is_handoff_ready(&tx),
	      "SCR idle did not carry line energy through S/S-bar");
	return true;
}

static bool run_rrn_continuation(bool enter_mid_point)
{
	struct v34_phase3_profile selected = profile();
	struct v34_training_line source;
	struct v34_training_line model_line;
	struct v34_training_line handoff;
	struct v90_phase3_return_line_tx tx;
	struct v90_phase3_return_tx model_points;
	uint32_t drained_samples = 0u;
	bool model_draining = enter_mid_point;
	bool complete = false;

	CHECK(v34_training_line_init(
		      &source, &selected, V90_PHASE3_SAMPLE_RATE,
		      NOMINAL_AMPLITUDE) == V34_TRAINING_LINE_OK,
	      "RRN source line did not initialize");
	if (enter_mid_point) {
		struct qam_point data_point = v34_rotated_point_zero(1);
		int16_t ignored;
		bool accepted;

		CHECK(v34_training_line_sample(
			      &source, &data_point, &ignored, &accepted) ==
			      V34_TRAINING_LINE_OK &&
		      accepted && !v34_training_line_needs_point(&source),
		      "RRN fixture did not enter an accepted data point");
	}
	model_line = source;
	CHECK(v90_phase3_return_line_tx_init_rrn(&tx, &source) ==
		      V90_PHASE3_RETURN_LINE_TX_OK &&
	      v90_phase3_return_tx_init(&model_points) ==
		      V90_PHASE3_RETURN_TX_OK &&
	      v90_phase3_return_tx_start_s(&model_points) ==
		      V90_PHASE3_RETURN_TX_OK &&
	      v90_phase3_return_tx_finish_s(&model_points) ==
		      V90_PHASE3_RETURN_TX_OK,
	      "automatic RRN return did not initialize");
	for (uint32_t sample_index = 0;
	     sample_index < SAMPLE_LIMIT; ++sample_index) {
		int16_t actual;
		int16_t expected;

		if (v90_phase3_return_line_tx_is_handoff_ready(&tx)) {
			complete = true;
			break;
		}
		if (model_draining) {
			bool accepted;

			CHECK(v34_training_line_sample(
				      &model_line, NULL, &expected,
				      &accepted) == V34_TRAINING_LINE_OK &&
			      !accepted,
			      "RRN model could not drain its accepted point");
			drained_samples++;
			model_draining =
				!v34_training_line_needs_point(&model_line);
		} else {
			CHECK(model_sample(
				      &model_line, &model_points, NULL,
				      &expected),
			      "RRN model could not emit S/S-bar");
		}
		CHECK(v90_phase3_return_line_tx_sample(&tx, &actual) ==
			      V90_PHASE3_RETURN_LINE_TX_OK &&
		      actual == expected &&
		      memcmp(&tx.line, &model_line, sizeof(model_line)) == 0 &&
		      memcmp(&tx.points, &model_points,
			     sizeof(model_points)) == 0,
		      "automatic RRN waveform diverged at sample %u",
		      sample_index);
	}
	CHECK(complete &&
	      model_points.s_symbols == V34_PHASE3_S_SYMBOLS &&
	      model_points.s_bar_symbols == V34_PHASE3_S_BAR_SYMBOLS &&
	      v90_phase3_return_line_tx_handoff(&tx, &handoff) ==
		      V90_PHASE3_RETURN_LINE_TX_OK &&
	      memcmp(&handoff, &model_line, sizeof(handoff)) == 0 &&
	      (enter_mid_point ? drained_samples > 0u :
				 drained_samples == 0u),
	      "automatic RRN did not preserve its exact 128T/16T handoff");
	{
		struct v90_phase3_return_line_tx saved = tx;
		int16_t sample = 4321;

		CHECK(v90_phase3_return_line_tx_sample(&tx, &sample) ==
			      V90_PHASE3_RETURN_LINE_TX_COMPLETE &&
		      v90_phase3_return_line_tx_start_s(&tx) ==
			      V90_PHASE3_RETURN_LINE_TX_INVALID_STATE &&
		      v90_phase3_return_line_tx_finish_s(&tx) ==
			      V90_PHASE3_RETURN_LINE_TX_INVALID_STATE &&
		      memcmp(&tx, &saved, sizeof(tx)) == 0 && sample == 4321,
		      "complete automatic RRN changed state or output");
	}
	return true;
}

static bool test_rate_renegotiation_continuation(void)
{
	return run_rrn_continuation(false) && run_rrn_continuation(true);
}

static bool test_validation(void)
{
	struct v34_phase3_profile selected = profile();
	struct v34_training_line line;
	struct v90_phase3_return_line_tx tx;
	struct v90_phase3_return_line_tx saved;
	int16_t sample = 777;

	CHECK(v34_training_line_init(
		      &line, &selected, V90_PHASE3_SAMPLE_RATE,
		      NOMINAL_AMPLITUDE) == V34_TRAINING_LINE_OK,
	      "validation line did not initialize");
	memset(&tx, 0xa5, sizeof(tx));
	saved = tx;
	line.profile.local_role = V34_CALLING_STATION;
	CHECK(v90_phase3_return_line_tx_init(&tx, &line) ==
		      V90_PHASE3_RETURN_LINE_TX_INVALID_ARGUMENT &&
	      v90_phase3_return_line_tx_init_rrn(&tx, &line) ==
		      V90_PHASE3_RETURN_LINE_TX_INVALID_ARGUMENT &&
	      memcmp(&tx, &saved, sizeof(tx)) == 0,
	      "return line accepted the digital role");
	CHECK(v34_training_line_init(
		      &line, &selected, V90_PHASE3_SAMPLE_RATE,
		      NOMINAL_AMPLITUDE) == V34_TRAINING_LINE_OK,
	      "valid return line source did not initialize");
	CHECK(v90_phase3_return_line_tx_init_rrn(NULL, &line) ==
		      V90_PHASE3_RETURN_LINE_TX_INVALID_ARGUMENT,
	      "automatic RRN accepted a NULL destination");
	saved = tx;
	CHECK(v90_phase3_return_line_tx_init_scr(
		      &tx, &line, (enum v34_constellation_size)99) ==
		      V90_PHASE3_RETURN_LINE_TX_INVALID_ARGUMENT &&
	      memcmp(&tx, &saved, sizeof(tx)) == 0,
	      "invalid SCR constellation changed return-line state");
	CHECK(v90_phase3_return_line_tx_init(&tx, &line) ==
		      V90_PHASE3_RETURN_LINE_TX_OK,
	      "valid return line did not initialize");
	saved = tx;
	CHECK(v90_phase3_return_line_tx_sample(NULL, &sample) ==
		      V90_PHASE3_RETURN_LINE_TX_INVALID_ARGUMENT &&
	      v90_phase3_return_line_tx_sample(&tx, NULL) ==
		      V90_PHASE3_RETURN_LINE_TX_INVALID_ARGUMENT &&
	      v90_phase3_return_line_tx_finish_s(&tx) ==
		      V90_PHASE3_RETURN_LINE_TX_INVALID_STATE &&
	      v90_phase3_return_line_tx_handoff(&tx, &line) ==
		      V90_PHASE3_RETURN_LINE_TX_INVALID_STATE &&
	      memcmp(&tx, &saved, sizeof(tx)) == 0 && sample == 777,
	      "invalid return-line operation changed state or output");
	tx.points.stage = (enum v90_phase3_return_tx_stage)99;
	CHECK(!v90_phase3_return_line_tx_is_valid(&tx) &&
	      !v90_phase3_return_line_tx_is_handoff_ready(&tx) &&
	      strcmp(v90_phase3_return_line_tx_result_name(
		     V90_PHASE3_RETURN_LINE_TX_INTERNAL_ERROR),
		     "internal error") == 0 &&
	      strcmp(v90_phase3_return_line_tx_result_name(
		     (enum v90_phase3_return_line_tx_result)99),
		     "unknown") == 0,
	      "return-line validation or names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "exact continuation", test_exact_continuation },
		{ "SCR idle continuation", test_scr_idle_continuation },
		{ "rate-renegotiation continuation",
		  test_rate_renegotiation_continuation },
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
