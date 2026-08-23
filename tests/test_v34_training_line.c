// SPDX-License-Identifier: BSD-3-Clause

#include <v34_training_line.h>

#include "test.h"

#include <limits.h>
#include <math.h>
#include <string.h>

static struct v34_phase3_profile profile_fixture(void)
{
	return (struct v34_phase3_profile){
		.symbol_rate_numerator = 3000,
		.symbol_rate_denominator = 1,
		.carrier_frequency_numerator = 2000,
		.carrier_frequency_denominator = 1,
		.symbol_rate = V34_SYMBOL_RATE_3000,
		.local_role = V34_CALLING_STATION,
		.preemphasis_filter_index = 8,
		.projected_data_rate_2400_bps_units = 1,
		.high_carrier = true,
	};
}

static struct qam_point generated_point(uint32_t index)
{
	static const struct qam_point points[] = {
		{ 0.60, 0.20 },
		{ -0.25, 0.55 },
		{ -0.60, -0.20 },
		{ 0.25, -0.55 },
		{ 0.80, -0.35 },
		{ -0.45, -0.70 },
	};

	return points[index % ARRAY_LENGTH(points)];
}

static bool test_continuous_points(void)
{
	struct v34_phase3_profile profile = profile_fixture();
	struct v34_training_line line;
	struct v34_pulse_modulator model_carrier;
	struct v34_preemphasis model_filter;
	uint32_t accepted_points = 0;
	uint32_t generated_samples = 0;

	CHECK(v34_training_line_init(&line, &profile, 8000, 5000) ==
		      V34_TRAINING_LINE_OK,
	      "continuous line did not initialize");
	model_carrier = line.carrier;
	model_filter = line.preemphasis;
	while (accepted_points < 512) {
		struct qam_point point = generated_point(accepted_points);
		const struct qam_point *supplied = NULL;
		const struct qam_point *model_point = NULL;
		double carrier_sample;
		double filtered_sample;
		long expected_sample;
		int16_t sample;
		bool needs_point = v34_training_line_needs_point(&line);
		bool model_accepted;
		bool accepted;

		CHECK(generated_samples < 2000,
		      "continuous line stopped accepting points");
		if (needs_point) {
			supplied = &point;
			model_point = &point;
		}
		CHECK(v34_pulse_modulator_sample(
			      &model_carrier, model_point, &carrier_sample,
			      &model_accepted) == V34_PULSE_MODULATOR_OK &&
		      v34_preemphasis_sample(
			      &model_filter, carrier_sample,
			      &filtered_sample) == V34_PREEMPHASIS_OK,
		      "independent waveform model failed at sample %u",
		      generated_samples);
		expected_sample = lround(filtered_sample * line.output_gain);
		CHECK(expected_sample >= INT16_MIN &&
		      expected_sample <= INT16_MAX &&
		      v34_training_line_sample(
			      &line, supplied, &sample, &accepted) ==
			      V34_TRAINING_LINE_OK &&
		      accepted == needs_point && accepted == model_accepted &&
		      sample == expected_sample &&
		      memcmp(&line.carrier, &model_carrier,
			     sizeof(model_carrier)) == 0 &&
		      memcmp(&line.preemphasis, &model_filter,
			     sizeof(model_filter)) == 0,
		      "continuous waveform diverged at sample %u",
		      generated_samples);
		if (accepted)
			accepted_points++;
		generated_samples++;
	}
	CHECK(line.generated_samples == generated_samples &&
	      accepted_points == 512 &&
	      v34_training_line_is_valid(&line),
	      "continuous line lost sample or point ownership");
	return true;
}

static bool advance_to_point_boundary(struct v34_training_line *line)
{
	while (!v34_training_line_needs_point(line)) {
		int16_t ignored;
		bool accepted;

		if (v34_training_line_sample(
			    line, NULL, &ignored, &accepted) !=
			    V34_TRAINING_LINE_OK ||
		    accepted)
			return false;
	}
	return true;
}

static bool test_transactional_contract(void)
{
	struct v34_phase3_profile profile = profile_fixture();
	struct v34_training_line line;
	struct v34_training_line before;
	struct qam_point point = generated_point(0);
	struct qam_point invalid_point = { NAN, 0.0 };
	int16_t sample = 1234;
	bool accepted = true;

	CHECK(v34_training_line_init(&line, &profile, 8000, 5000) ==
		      V34_TRAINING_LINE_OK &&
	      v34_training_line_needs_point(&line),
	      "transaction fixture did not initialize at a point boundary");
	before = line;
	CHECK(v34_training_line_sample(
		      &line, NULL, &sample, &accepted) ==
		      V34_TRAINING_LINE_INVALID_ARGUMENT &&
	      memcmp(&line, &before, sizeof(line)) == 0 && sample == 1234 &&
	      accepted,
	      "a missing point mutated state or outputs");
	CHECK(v34_training_line_sample(
		      &line, &point, &sample, &accepted) ==
		      V34_TRAINING_LINE_OK &&
	      accepted && !v34_training_line_needs_point(&line),
	      "the first point was not accepted exactly once");
	before = line;
	sample = 1234;
	accepted = true;
	CHECK(v34_training_line_sample(
		      &line, &point, &sample, &accepted) ==
		      V34_TRAINING_LINE_INVALID_ARGUMENT &&
	      memcmp(&line, &before, sizeof(line)) == 0 && sample == 1234 &&
	      accepted,
	      "an early point mutated state or outputs");
	CHECK(advance_to_point_boundary(&line),
	      "line did not reach its next point boundary");
	before = line;
	sample = 1234;
	accepted = true;
	CHECK(v34_training_line_sample(
		      &line, &invalid_point, &sample, &accepted) ==
		      V34_TRAINING_LINE_INVALID_ARGUMENT &&
	      memcmp(&line, &before, sizeof(line)) == 0 && sample == 1234 &&
	      accepted,
	      "a non-finite point mutated state or outputs");
	CHECK(v34_training_line_sample(
		      &line, &point, NULL, &accepted) ==
		      V34_TRAINING_LINE_INVALID_ARGUMENT &&
	      v34_training_line_sample(
		      &line, &point, &sample, NULL) ==
		      V34_TRAINING_LINE_INVALID_ARGUMENT &&
	      memcmp(&line, &before, sizeof(line)) == 0,
	      "a null output mutated line state");
	return true;
}

static bool test_initialization_and_overflow(void)
{
	struct v34_phase3_profile profile = profile_fixture();
	struct v34_phase3_profile invalid_profile = profile;
	struct v34_training_line line;
	struct v34_training_line before;
	struct qam_point point = generated_point(0);
	const struct qam_point *supplied;
	int16_t sample = 1234;
	bool accepted = true;

	memset(&line, 0xa5, sizeof(line));
	before = line;
	invalid_profile.preemphasis_filter_index = 11;
	CHECK(v34_training_line_init(NULL, &profile, 8000, 5000) ==
		      V34_TRAINING_LINE_INVALID_ARGUMENT &&
	      v34_training_line_init(&line, NULL, 8000, 5000) ==
		      V34_TRAINING_LINE_INVALID_ARGUMENT &&
	      v34_training_line_init(
		      &line, &invalid_profile, 8000, 5000) ==
		      V34_TRAINING_LINE_INVALID_ARGUMENT &&
	      v34_training_line_init(&line, &profile, 7000, 5000) ==
		      V34_TRAINING_LINE_OUT_OF_RANGE &&
	      v34_training_line_init(&line, &profile, 8000, 0) ==
		      V34_TRAINING_LINE_INVALID_ARGUMENT &&
	      memcmp(&line, &before, sizeof(line)) == 0,
	      "invalid initialization mutated its destination");
	CHECK(v34_training_line_init(&line, &profile, 8000, INT16_MAX) ==
		      V34_TRAINING_LINE_CLIPPING_RISK &&
	      memcmp(&line, &before, sizeof(line)) == 0,
	      "a clipping calibration was accepted or mutated its destination");
	CHECK(v34_training_line_init(&line, &profile, 8000, 5000) ==
		      V34_TRAINING_LINE_OK,
	      "overflow fixture did not initialize");
	line.generated_samples = UINT64_MAX;
	line.carrier.generated_samples = UINT64_MAX;
	line.preemphasis.next_history =
		(size_t)(UINT64_MAX % V34_PREEMPHASIS_TAPS);
	line.carrier.carrier_phase =
		(uint32_t)UINT64_MAX * line.carrier.carrier_phase_step;
	line.carrier.symbol_phase_units =
		(UINT64_MAX % line.carrier.symbol_period_units) *
		profile.symbol_rate_numerator %
		line.carrier.symbol_period_units;
	CHECK(v34_training_line_is_valid(&line),
	      "representable overflow fixture was not valid");
	supplied = v34_training_line_needs_point(&line) ? &point : NULL;
	before = line;
	CHECK(v34_training_line_sample(
		      &line, supplied, &sample, &accepted) ==
		      V34_TRAINING_LINE_OUT_OF_RANGE &&
	      memcmp(&line, &before, sizeof(line)) == 0 && sample == 1234 &&
	      accepted,
	      "sample-count overflow mutated state or outputs");
	CHECK(!v34_training_line_is_valid(NULL) &&
	      !v34_training_line_needs_point(NULL) &&
	      strcmp(v34_training_line_result_name(
		     V34_TRAINING_LINE_CLIPPING_RISK),
		     "clipping risk") == 0 &&
	      strcmp(v34_training_line_result_name(
		     (enum v34_training_line_result)99),
		     "unknown") == 0,
	      "training-line validation or result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "continuous points", test_continuous_points },
		{ "transactional contract", test_transactional_contract },
		{ "initialization and overflow", test_initialization_and_overflow },
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
