// SPDX-License-Identifier: BSD-3-Clause

#include <v34_training_line.h>

#include "test.h"

#include <float.h>
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

static bool test_calibrated_line_power(void)
{
	struct v34_info1_probe_result probe = {
		.high_carrier = true,
		.projected_data_rate_2400_bps_units = 1,
	};
	double full_power =
		V34_TRAINING_LINE_REFERENCE_AMPLITUDE / sqrt(2.0);

	for (unsigned int rate = 0; rate < V34_SYMBOL_RATE_COUNT; ++rate) {
		for (uint8_t preemphasis = 0;
		     preemphasis <= V34_INFO1_MAX_PREEMPHASIS_FILTER_INDEX;
		     ++preemphasis) {
			struct v34_phase3_profile profile;
			struct v34_training_line line;

			probe.preemphasis_filter_index = preemphasis;
			CHECK(v34_phase3_profile_init(
				      &profile, V34_CALLING_STATION,
				      (enum v34_symbol_rate)rate, &probe, 0, 0) ==
				      V34_PHASE3_PROFILE_OK &&
			      v34_training_line_init(
				      &line, &profile, 8000,
				      V34_TRAINING_LINE_REFERENCE_AMPLITUDE) ==
				      V34_TRAINING_LINE_OK &&
			      fabs(line.target_rms - full_power) <= 1e-9 &&
			      fabs(line.output_gain * line.raw_rms - full_power) <=
				      1e-9 &&
			      line.raw_peak * line.output_gain < INT16_MAX,
			      "line calibration changed at rate %u/pre-emphasis %u",
			      rate, preemphasis);
		}
	}
	for (uint8_t reduction = 0;
	     reduction <= V34_INFO1_MAX_POWER_REDUCTION_DB; ++reduction) {
		struct v34_phase3_profile profile;
		struct v34_training_line line;
		double expected_power =
			full_power * pow(10.0, -(double)reduction / 20.0);

		probe.preemphasis_filter_index = 7;
		CHECK(v34_phase3_profile_init(
			      &profile, V34_ANSWERING_STATION,
			      V34_SYMBOL_RATE_3200, &probe, reduction, 0) ==
			      V34_PHASE3_PROFILE_OK &&
		      v34_training_line_init(
			      &line, &profile, 8000,
			      V34_TRAINING_LINE_REFERENCE_AMPLITUDE) ==
			      V34_TRAINING_LINE_OK &&
		      fabs(line.target_rms - expected_power) <= 1e-9 &&
		      fabs(line.output_gain * line.raw_rms - expected_power) <=
			      1e-9,
		      "calibrated %u dB line reduction changed", reduction);
	}
	return true;
}

static bool continuous_points(const struct v34_phase3_profile *profile)
{
	struct v34_training_line line;
	struct v34_pulse_modulator model_carrier;
	uint32_t accepted_points = 0;
	uint32_t generated_samples = 0;

	CHECK(v34_training_line_init(&line, profile, 8000, 5000) ==
		      V34_TRAINING_LINE_OK,
	      "continuous line did not initialize");
	model_carrier = line.carrier;
	while (accepted_points < 512) {
		struct qam_point point = generated_point(accepted_points);
		const struct qam_point *supplied = NULL;
		const struct qam_point *model_point = NULL;
		double line_sample;
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
			      &model_carrier, model_point, &line_sample,
			      &model_accepted) == V34_PULSE_MODULATOR_OK,
		      "independent waveform model failed at sample %u",
		      generated_samples);
		expected_sample = lround(line_sample * line.output_gain);
		CHECK(expected_sample >= INT16_MIN &&
		      expected_sample <= INT16_MAX &&
		      v34_training_line_sample(
			      &line, supplied, &sample, &accepted) ==
			      V34_TRAINING_LINE_OK &&
		      accepted == needs_point && accepted == model_accepted &&
		      sample == expected_sample &&
		      line.generated_samples == generated_samples + 1u &&
		      v34_training_line_is_valid(&line) &&
		      memcmp(&line.carrier, &model_carrier,
			     sizeof(model_carrier)) == 0,
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

static bool test_continuous_points(void)
{
	struct v34_info1_probe_result probe = {
		.projected_data_rate_2400_bps_units = 1,
	};
	for (unsigned int rate = 0; rate < V34_SYMBOL_RATE_COUNT; ++rate) {
		for (unsigned int high = 0; high < 2; ++high) {
			if (rate == V34_SYMBOL_RATE_3429 && !high)
				continue;
			for (unsigned int filter = 0; filter <= 8; filter += 8) {
				struct v34_phase3_profile profile;

				probe.high_carrier = high != 0;
				probe.preemphasis_filter_index = (uint8_t)filter;
				CHECK(v34_phase3_profile_init(&profile,
					      V34_CALLING_STATION,
					      (enum v34_symbol_rate)rate,
					      &probe, 0, 0) == V34_PHASE3_PROFILE_OK &&
				      continuous_points(&profile),
				      "continuous line changed at %u/%u/%u",
				      rate, high, filter);
			}
		}
	}
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
	line.generated_samples = UINT64_MAX - 1u;
	line.carrier.generated_samples = line.generated_samples;
	line.carrier.carrier_phase =
		(uint32_t)line.generated_samples * line.carrier.carrier_phase_step;
	line.carrier.symbol_phase_units =
		(line.generated_samples % line.carrier.symbol_period_units) *
		profile.symbol_rate_numerator %
		line.carrier.symbol_period_units;
	CHECK(v34_training_line_is_valid(&line),
	      "representable overflow fixture was not valid");
	supplied = v34_training_line_needs_point(&line) ? &point : NULL;
	CHECK(v34_training_line_sample(&line, supplied, &sample, &accepted) ==
		      V34_TRAINING_LINE_OK &&
	      line.generated_samples == UINT64_MAX &&
	      line.carrier.generated_samples == UINT64_MAX &&
	      v34_training_line_is_valid(&line),
	      "the final representable sample lost its clock or count");
	supplied = v34_training_line_needs_point(&line) ? &point : NULL;
	before = line;
	sample = 1234;
	accepted = true;
	CHECK(v34_training_line_sample(
		      &line, supplied, &sample, &accepted) ==
		      V34_TRAINING_LINE_OUT_OF_RANGE &&
	      memcmp(&line, &before, sizeof(line)) == 0 && sample == 1234 &&
	      accepted,
	      "sample-count overflow mutated state or outputs");
	CHECK(!v34_training_line_is_valid(NULL) &&
	      !v34_training_line_needs_point(NULL),
	      "training-line validation changed");
	return true;
}

static bool test_disjoint_buffers(void)
{
	struct v34_phase3_profile profile = profile_fixture();
	struct v34_training_line line;
	struct qam_point point = generated_point(0);
	struct { int16_t sample; bool accepted; } output = { 1234, true };
	const struct {
		const struct qam_point *point;
		int16_t *sample;
		bool *accepted;
	} overlapping[] = {
		{ &point, &line.nominal_peak_amplitude, &output.accepted },
		{ &point, &output.sample, &line.profile.high_carrier },
		{ (const struct qam_point *)&line.output_gain,
		  &output.sample, &output.accepted },
		{ &point, (int16_t *)&point.in_phase, &output.accepted },
		{ &point, &output.sample, (bool *)&point.in_phase },
		{ &point, &output.sample, (bool *)&output.sample },
		{ &point, &output.sample,
		  (bool *)((unsigned char *)&output.sample + sizeof(int16_t) - 1) },
	};

	CHECK(v34_training_line_init(&line, &profile, 8000, 5000) ==
		      V34_TRAINING_LINE_OK,
	      "overlap fixture did not initialize");
	for (unsigned int index = 0; index < 64; ++index)
		CHECK(v34_training_line_sample(&line,
			      v34_training_line_needs_point(&line) ? &point : NULL,
			      &output.sample, &output.accepted) ==
			      V34_TRAINING_LINE_OK,
		      "overlap fixture did not warm up");
	CHECK(advance_to_point_boundary(&line), "overlap fixture stalled");
	output.sample = 1234;
	output.accepted = true;
	for (size_t index = 0; index < ARRAY_LENGTH(overlapping); ++index) {
		struct v34_training_line before = line;
		struct qam_point point_before = point;

		/* Casted regions are rejected before they are dereferenced. */
		CHECK(v34_training_line_sample(&line, overlapping[index].point,
			      overlapping[index].sample, overlapping[index].accepted) ==
			      V34_TRAINING_LINE_INVALID_ARGUMENT &&
		      memcmp(&line, &before, sizeof(line)) == 0 &&
		      memcmp(&point, &point_before, sizeof(point)) == 0 &&
		      output.sample == 1234 && output.accepted,
		      "overlapping buffers %zu mutated state or outputs", index);
	}
	CHECK(v34_training_line_sample(&line, &point,
		      &output.sample, &output.accepted) == V34_TRAINING_LINE_OK &&
	      output.accepted && v34_training_line_is_valid(&line),
	      "adjacent disjoint outputs were rejected");
	struct v34_training_line before = line;
	CHECK(!v34_training_line_needs_point(&line) &&
	      v34_training_line_sample(&line, NULL, &line.nominal_peak_amplitude,
		      &output.accepted) == V34_TRAINING_LINE_INVALID_ARGUMENT &&
	      v34_training_line_sample(&line, NULL, &output.sample,
		      &line.profile.high_carrier) ==
		      V34_TRAINING_LINE_INVALID_ARGUMENT &&
	      memcmp(&line, &before, sizeof(line)) == 0,
	      "no-point sample accepted overlapping owner outputs");
	return true;
}

static bool test_late_sample_rollback(void)
{
	struct v34_phase3_profile profile = profile_fixture();
	struct v34_training_line line;
	struct qam_point point = { 1.34, 0.0 };
	const double gains[] = { 1e8, DBL_MAX };
	const struct qam_point *supplied;
	double raw;
	int16_t sample;
	bool accepted;

	profile.preemphasis_filter_index = 0;
	CHECK(v34_training_line_init(&line, &profile, 8000, 5000) ==
		      V34_TRAINING_LINE_OK,
	      "late sample fixture did not initialize");
	for (unsigned int index = 0; index < 64; ++index) {
		supplied = v34_training_line_needs_point(&line) ? &point : NULL;
		CHECK(v34_training_line_sample(&line, supplied, &sample,
			      &accepted) == V34_TRAINING_LINE_OK,
		      "late sample fixture did not warm up");
	}
	for (unsigned int interval = 0; interval < 2; ++interval) {
		/* The public owner accepts any int16 filter history. The identity
		 * pre-emphasis emits history[6] next, after shifting it to tap 7. */
		line.carrier.preemphasis_history[6] = 30000;
		struct v34_pulse_modulator model = line.carrier;
		supplied = v34_training_line_needs_point(&line) ? &point : NULL;
		CHECK((supplied != NULL) == (interval == 0) &&
		      v34_pulse_modulator_sample(&model, supplied, &raw, &accepted) ==
			      V34_PULSE_MODULATOR_OK && raw == 30000.0 / 16384.0,
		      "late sample pulse model changed");
		for (size_t gain = 0; gain < ARRAY_LENGTH(gains); ++gain) {
			struct v34_training_line bad = line;

			/* Valid calibration metadata does not bound all history. */
			bad.output_gain = gains[gain];
			bad.raw_rms = bad.target_rms / bad.output_gain;
			bad.raw_peak = bad.raw_rms;
			struct v34_training_line before = bad;

			CHECK(v34_training_line_is_valid(&bad),
			      "late error fixture must start valid and warmed");
			for (unsigned int retry = 0; retry < 2; ++retry) {
				sample = 1234;
				accepted = true;
				CHECK(v34_training_line_sample(&bad, supplied, &sample,
					      &accepted) == (gain == 0 ?
					      V34_TRAINING_LINE_CLIPPING_RISK :
					      V34_TRAINING_LINE_NUMERIC_FAILURE) &&
				      memcmp(&bad, &before, sizeof(bad)) == 0 &&
				      sample == 1234 && accepted,
				      "late error %zu lost rollback on retry", gain);
			}
		}
		CHECK(v34_training_line_sample(&line, supplied, &sample, &accepted) ==
			      V34_TRAINING_LINE_OK &&
		      memcmp(&line.carrier, &model, sizeof(model)) == 0 &&
		      v34_training_line_is_valid(&line),
		      "representable retry changed the waveform");
	}
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "calibrated line power", test_calibrated_line_power },
		{ "continuous points", test_continuous_points },
		{ "transactional contract", test_transactional_contract },
		{ "initialization and overflow", test_initialization_and_overflow },
		{ "disjoint sample buffers", test_disjoint_buffers },
		{ "late sample rollback", test_late_sample_rollback },
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
