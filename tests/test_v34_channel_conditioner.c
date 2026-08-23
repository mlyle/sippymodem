// SPDX-License-Identifier: BSD-3-Clause

#include <v34_channel_conditioner.h>

#include "test.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

static bool nearly_equal(double left, double right)
{
	double scale = fmax(1.0, fmax(fabs(left), fabs(right)));

	return fabs(left - right) <= 3e-13 * scale;
}

static bool initialize_normalizer(
	enum v34_nonlinear_parameter nonlinear_parameter,
	struct v34_power_normalizer *normalizer)
{
	struct v34_frame_geometry geometry;
	struct v34_mapping_parameters parameters;
	const struct v34_symbol_power_measurement precoded = {
		.mean_squared_magnitude = 25.0,
		.symbol_count = 256,
	};
	struct v34_symbol_power_measurement projected = {
		.mean_squared_magnitude = 30.0,
		.symbol_count = 256,
	};

	if (nonlinear_parameter == V34_NONLINEAR_THETA_ZERO)
		projected.mean_squared_magnitude =
			precoded.mean_squared_magnitude;
	return v34_frame_geometry_init(
		       &geometry, V34_SYMBOL_RATE_3000, 8, false) ==
		       V34_FRAME_SCHEDULE_OK &&
	       v34_mapping_parameters_init(
		       &parameters, &geometry, false) ==
		       V34_MAPPING_PARAMETERS_OK &&
	       v34_power_normalizer_init(
		       normalizer, &parameters, nonlinear_parameter, &precoded,
		       &projected) == V34_SYMBOL_POWER_OK;
}

static uint32_t next_random(uint32_t *state)
{
	*state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
	return *state;
}

static bool test_generated_round_trip(void)
{
	uint32_t random = UINT32_C(0xa305e4d1);

	for (enum v34_nonlinear_parameter nonlinear_parameter =
		     V34_NONLINEAR_THETA_ZERO;
	     nonlinear_parameter <=
	     V34_NONLINEAR_THETA_5_OVER_16;
	     ++nonlinear_parameter) {
		struct v34_power_normalizer normalizer;
		struct v34_channel_conditioner conditioner;

		CHECK(initialize_normalizer(
			      nonlinear_parameter, &normalizer) &&
		      v34_channel_conditioner_init(
			      &conditioner, &normalizer) ==
			      V34_CHANNEL_CONDITIONER_OK,
		      "conditioner mode %u initialization failed",
		      nonlinear_parameter);
		for (unsigned int sample = 0; sample < 1024; ++sample) {
			struct v34_q7_point input = {
				.in_phase =
					(int32_t)(next_random(&random) % 2049u) - 1024,
				.quadrature =
					(int32_t)(next_random(&random) % 2049u) - 1024,
			};
			struct qam_point projected;
			struct qam_point normalized;
			struct qam_point actual;
			double expected_in_phase = (double)input.in_phase /
				V34_Q7_SCALE;
			double expected_quadrature = (double)input.quadrature /
				V34_Q7_SCALE;

			CHECK(v34_nonlinear_encode(
				      nonlinear_parameter,
				      normalizer.precoded_mean_squared_magnitude,
				      &input, &projected) ==
				      V34_NONLINEAR_OK &&
			      v34_normalize_power(
				      &normalizer, &projected, &normalized) ==
				      V34_SYMBOL_POWER_OK &&
			      v34_channel_condition_point(
				      &conditioner, &normalized, &actual) ==
				      V34_CHANNEL_CONDITIONER_OK &&
			      nearly_equal(actual.in_phase, expected_in_phase) &&
			      nearly_equal(
				      actual.quadrature, expected_quadrature),
			      "conditioner mode %u sample %u did not round trip",
			      nonlinear_parameter, sample);
		}
	}
	return true;
}

static bool prepare_normalized_point(
	const struct v34_power_normalizer *normalizer,
	const struct v34_q7_point *input,
	struct qam_point *normalized)
{
	struct qam_point projected;

	return v34_nonlinear_encode(
		       normalizer->nonlinear_parameter,
		       normalizer->precoded_mean_squared_magnitude, input,
		       &projected) == V34_NONLINEAR_OK &&
	       v34_normalize_power(
		       normalizer, &projected, normalized) ==
		       V34_SYMBOL_POWER_OK;
}

static bool test_interval_and_noise(void)
{
	static const struct v34_q7_point INPUTS[] = {
		{ 3 * V34_Q7_SCALE,
		  -4 * V34_Q7_SCALE },
		{ -7 * V34_Q7_SCALE + 5,
		  2 * V34_Q7_SCALE - 11 },
	};
	struct v34_power_normalizer normalizer;
	struct v34_channel_conditioner conditioner;
	struct qam_point normalized[2];
	struct qam_point actual[2];
	struct qam_point in_place[2];
	struct qam_point broken[2];
	struct qam_point unchanged[2] = {
		{ 123.0, -456.0 },
		{ 789.0, -321.0 },
	};
	struct qam_point saved[2];
	struct qam_point noisy;
	struct qam_point noisy_estimate;
	double maximum_axis;

	CHECK(initialize_normalizer(
		      V34_NONLINEAR_THETA_5_OVER_16,
		      &normalizer) &&
	      v34_channel_conditioner_init(
		      &conditioner, &normalizer) ==
		      V34_CHANNEL_CONDITIONER_OK,
	      "interval conditioner initialization failed");
	for (size_t symbol = 0; symbol < ARRAY_LENGTH(INPUTS); ++symbol)
		CHECK(prepare_normalized_point(
			      &normalizer, &INPUTS[symbol],
			      &normalized[symbol]),
		      "interval symbol %zu preparation failed", symbol);
	CHECK(v34_channel_condition_interval(
		      &conditioner, normalized, actual) ==
		      V34_CHANNEL_CONDITIONER_OK,
	      "interval conditioning failed");
	for (size_t symbol = 0; symbol < ARRAY_LENGTH(INPUTS); ++symbol) {
		CHECK(nearly_equal(
			      actual[symbol].in_phase,
			      (double)INPUTS[symbol].in_phase /
				      V34_Q7_SCALE) &&
		      nearly_equal(
			      actual[symbol].quadrature,
			      (double)INPUTS[symbol].quadrature /
				      V34_Q7_SCALE),
		      "interval symbol %zu changed", symbol);
		in_place[symbol] = normalized[symbol];
	}
	CHECK(v34_channel_condition_interval(
		      &conditioner, in_place, in_place) ==
		      V34_CHANNEL_CONDITIONER_OK &&
	      memcmp(in_place, actual, sizeof(actual)) == 0,
	      "in-place interval conditioning changed results");
	memcpy(broken, normalized, sizeof(broken));
	broken[1].quadrature = NAN;
	memcpy(saved, unchanged, sizeof(saved));
	CHECK(v34_channel_condition_interval(
		      &conditioner, broken, unchanged) ==
		      V34_CHANNEL_CONDITIONER_INVALID_ARGUMENT &&
	      memcmp(unchanged, saved, sizeof(saved)) == 0,
	      "failed interval conditioning committed partial output");
	maximum_axis =
		4.0 * sqrt(normalizer.maximum_normalized_point_power);
	noisy = (struct qam_point){
		.in_phase = maximum_axis,
		.quadrature = -0.75 * maximum_axis,
	};
	CHECK(noisy.in_phase * noisy.in_phase +
		      noisy.quadrature * noisy.quadrature >
		      normalizer.maximum_normalized_point_power &&
	      v34_channel_condition_point(
		      &conditioner, &noisy, &noisy_estimate) ==
		      V34_CHANNEL_CONDITIONER_OK &&
	      isfinite(noisy_estimate.in_phase) &&
	      isfinite(noisy_estimate.quadrature),
	      "finite noisy point was mistaken for transmit output");
	return true;
}

static bool test_validation(void)
{
	struct v34_power_normalizer normalizer;
	struct v34_power_normalizer damaged;
	struct v34_channel_conditioner conditioner;
	struct v34_channel_conditioner saved_conditioner;
	struct qam_point input = { 1.0, -2.0 };
	struct qam_point output = { 123.0, -456.0 };
	struct qam_point interval[2] = {
		{ 1.0, -2.0 },
		{ 3.0, -4.0 },
	};
	const struct qam_point saved_output = output;

	CHECK(initialize_normalizer(
		      V34_NONLINEAR_THETA_ZERO, &normalizer) &&
	      v34_channel_conditioner_init(
		      &conditioner, &normalizer) ==
		      V34_CHANNEL_CONDITIONER_OK,
	      "validation conditioner initialization failed");
	saved_conditioner = conditioner;
	damaged = normalizer;
	damaged.normalization_gain = 0.0;
	CHECK(!v34_channel_conditioner_is_valid(NULL) &&
	      !v34_power_normalizer_is_valid(&damaged) &&
	      v34_channel_conditioner_init(NULL, &normalizer) ==
		      V34_CHANNEL_CONDITIONER_INVALID_ARGUMENT &&
	      v34_channel_conditioner_init(
		      &conditioner, NULL) ==
		      V34_CHANNEL_CONDITIONER_INVALID_ARGUMENT &&
	      v34_channel_conditioner_init(
		      &conditioner, &damaged) ==
		      V34_CHANNEL_CONDITIONER_INVALID_ARGUMENT &&
	      memcmp(&conditioner, &saved_conditioner,
		     sizeof(conditioner)) == 0,
	      "invalid conditioner initialization mutated its destination");
	normalizer.normalization_gain = 0.0;
	CHECK(v34_channel_conditioner_is_valid(&conditioner),
	      "conditioner retained borrowed normalization state");
	CHECK(v34_channel_condition_point(
		      NULL, &input, &output) ==
		      V34_CHANNEL_CONDITIONER_INVALID_ARGUMENT &&
	      v34_channel_condition_point(
		      &conditioner, NULL, &output) ==
		      V34_CHANNEL_CONDITIONER_INVALID_ARGUMENT &&
	      v34_channel_condition_point(
		      &conditioner, &input, NULL) ==
		      V34_CHANNEL_CONDITIONER_INVALID_ARGUMENT &&
	      v34_channel_condition_point(
		      &conditioner,
		      &(struct qam_point){ NAN, 0.0 }, &output) ==
		      V34_CHANNEL_CONDITIONER_INVALID_ARGUMENT &&
	      v34_channel_condition_point(
		      &conditioner,
		      &(struct qam_point){ 0.0, INFINITY },
		      &output) ==
		      V34_CHANNEL_CONDITIONER_INVALID_ARGUMENT &&
	      output.in_phase == saved_output.in_phase &&
	      output.quadrature == saved_output.quadrature,
	      "invalid point conditioning mutated its destination");
	CHECK(v34_channel_condition_point(
		      &conditioner,
		      &(struct qam_point){ DBL_MAX, DBL_MAX },
		      &output) ==
		      V34_CHANNEL_CONDITIONER_NUMERIC_FAILURE &&
	      output.in_phase == saved_output.in_phase &&
	      output.quadrature == saved_output.quadrature,
	      "overflowing point conditioning mutated its destination");
	CHECK(v34_channel_condition_interval(
		      &conditioner, NULL, interval) ==
		      V34_CHANNEL_CONDITIONER_INVALID_ARGUMENT &&
	      v34_channel_condition_interval(
		      &conditioner, interval, NULL) ==
		      V34_CHANNEL_CONDITIONER_INVALID_ARGUMENT,
	      "invalid interval arguments were accepted");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "generated round trip", test_generated_round_trip },
		{ "interval and noise", test_interval_and_noise },
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
