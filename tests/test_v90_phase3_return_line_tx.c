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
	struct v90_phase3_return_tx *points, int16_t *sample)
{
	struct qam_point point;
	const struct qam_point *supplied = NULL;
	bool accepting = v34_training_line_needs_point(line);
	bool accepted;

	if (accepting) {
		if (v90_phase3_return_tx_next(points, &point) !=
		    V90_PHASE3_RETURN_TX_OK)
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
		CHECK(model_sample(&model_line, &model_points, &expected) &&
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
	      memcmp(&tx, &saved, sizeof(tx)) == 0,
	      "return line accepted the digital role");
	CHECK(v34_training_line_init(
		      &line, &selected, V90_PHASE3_SAMPLE_RATE,
		      NOMINAL_AMPLITUDE) == V34_TRAINING_LINE_OK &&
	      v90_phase3_return_line_tx_init(&tx, &line) ==
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
