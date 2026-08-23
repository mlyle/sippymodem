// SPDX-License-Identifier: BSD-3-Clause

#include <v34_nonlinear.h>

#include "test.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <string.h>

static bool nearly_equal(double actual, long double expected)
{
	long double scale = fmaxl(1.0L, fabsl(expected));

	return fabsl((long double)actual - expected) <= 2e-14L * scale;
}

static struct qam_point reference_projection(
	const struct v34_q7_point *input,
	double mean_squared_magnitude)
{
	long double in_phase =
		(long double)input->in_phase / V34_Q7_SCALE;
	long double quadrature =
		(long double)input->quadrature / V34_Q7_SCALE;
	long double energy =
		in_phase * in_phase + quadrature * quadrature;
	long double zeta = 5.0L * energy /
		(16.0L * (long double)mean_squared_magnitude);
	long double projection =
		1.0L + zeta / 6.0L + zeta * zeta / 120.0L;

	return (struct qam_point){
		.in_phase = (double)(in_phase * projection),
		.quadrature = (double)(quadrature * projection),
	};
}

static bool test_normative_projection(void)
{
	static const struct {
		struct v34_q7_point input;
		double mean_squared_magnitude;
	} cases[] = {
		{ { 0, 0 }, 25.0 },
		{ { 3 * V34_Q7_SCALE,
		    4 * V34_Q7_SCALE }, 25.0 },
		{ { -6 * V34_Q7_SCALE,
		    8 * V34_Q7_SCALE }, 25.0 },
		{ { 17 * V34_Q7_SCALE + 3,
		    -9 * V34_Q7_SCALE - 61 }, 419.25 },
	};

	for (size_t index = 0; index < ARRAY_LENGTH(cases); ++index) {
		struct qam_point expected = reference_projection(
			&cases[index].input,
			cases[index].mean_squared_magnitude);
		struct qam_point actual;
		struct qam_point unprojected;
		double projected_power;
		long double expected_power =
			(long double)expected.in_phase * expected.in_phase +
			(long double)expected.quadrature * expected.quadrature;
		double input_in_phase = (double)cases[index].input.in_phase /
			V34_Q7_SCALE;
		double input_quadrature =
			(double)cases[index].input.quadrature /
			V34_Q7_SCALE;
		double input_power = input_in_phase * input_in_phase +
			input_quadrature * input_quadrature;

		CHECK(v34_nonlinear_encode(
			      V34_NONLINEAR_THETA_5_OVER_16,
			      cases[index].mean_squared_magnitude,
			      &cases[index].input, &actual) ==
			      V34_NONLINEAR_OK &&
		      nearly_equal(actual.in_phase, expected.in_phase) &&
		      nearly_equal(actual.quadrature, expected.quadrature) &&
		      v34_nonlinear_project_power(
			      V34_NONLINEAR_THETA_5_OVER_16,
			      cases[index].mean_squared_magnitude, input_power,
			      &projected_power) ==
			      V34_NONLINEAR_OK &&
		      nearly_equal(projected_power, expected_power) &&
		      v34_nonlinear_unproject(
			      V34_NONLINEAR_THETA_5_OVER_16,
			      cases[index].mean_squared_magnitude, &actual,
			      &unprojected) == V34_NONLINEAR_OK &&
		      nearly_equal(unprojected.in_phase, input_in_phase) &&
		      nearly_equal(unprojected.quadrature, input_quadrature),
		      "non-linear projection case %zu changed", index);
	}
	return true;
}

static bool test_disabled_projection(void)
{
	static const struct v34_q7_point inputs[] = {
		{ 0, 0 },
		{ 1, -1 },
		{ 127, -129 },
		{ INT32_MAX, INT32_MIN },
	};

	for (size_t index = 0; index < ARRAY_LENGTH(inputs); ++index) {
		struct qam_point output;
		struct qam_point unprojected;
		double in_phase = (double)inputs[index].in_phase /
			V34_Q7_SCALE;
		double quadrature = (double)inputs[index].quadrature /
			V34_Q7_SCALE;
		double power = in_phase * in_phase + quadrature * quadrature;
		double projected_power = -1.0;

		CHECK(v34_nonlinear_encode(
			      V34_NONLINEAR_THETA_ZERO, 1.0,
			      &inputs[index], &output) ==
			      V34_NONLINEAR_OK &&
		      output.in_phase ==
			      (double)inputs[index].in_phase /
				      V34_Q7_SCALE &&
		      output.quadrature ==
			      (double)inputs[index].quadrature /
				      V34_Q7_SCALE &&
		      v34_nonlinear_project_power(
			      V34_NONLINEAR_THETA_ZERO, 1.0,
			      power, &projected_power) ==
			      V34_NONLINEAR_OK &&
		      projected_power == power &&
		      v34_nonlinear_unproject(
			      V34_NONLINEAR_THETA_ZERO, 1.0,
			      &output, &unprojected) ==
			      V34_NONLINEAR_OK &&
		      unprojected.in_phase == output.in_phase &&
		      unprojected.quadrature == output.quadrature,
		      "zero-theta projection case %zu changed", index);
	}
	return true;
}

static uint32_t next_random(uint32_t *state)
{
	*state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
	return *state;
}

static bool test_generated_sweep(void)
{
	static const double MEAN_ENERGIES[] = {
		0.5,
		25.0,
		1024.0,
		2.0 * V34_CHANNEL_COORDINATE_LIMIT *
			V34_CHANNEL_COORDINATE_LIMIT,
	};
	uint32_t random = UINT32_C(0x46a091bf);
	size_t nonzero_count = 0;

	for (size_t energy_index = 0;
	     energy_index < ARRAY_LENGTH(MEAN_ENERGIES); ++energy_index) {
		for (unsigned int sample = 0; sample < 512; ++sample) {
			struct v34_q7_point input = {
				.in_phase = (int32_t)(next_random(&random) % 65281u) -
					32768,
				.quadrature =
					(int32_t)(next_random(&random) % 65281u) -
					32768,
			};
			struct qam_point expected =
				reference_projection(
					&input, MEAN_ENERGIES[energy_index]);
			struct qam_point actual;
			struct qam_point unprojected;
			double in_phase = (double)input.in_phase /
				V34_Q7_SCALE;
			double quadrature = (double)input.quadrature /
				V34_Q7_SCALE;
			double input_power = in_phase * in_phase +
				quadrature * quadrature;
			double projected_power;
			long double expected_power =
				(long double)expected.in_phase * expected.in_phase +
				(long double)expected.quadrature * expected.quadrature;
			long double cross_product;

			CHECK(v34_nonlinear_encode(
				      V34_NONLINEAR_THETA_5_OVER_16,
				      MEAN_ENERGIES[energy_index], &input,
				      &actual) ==
				      V34_NONLINEAR_OK &&
			      nearly_equal(actual.in_phase, expected.in_phase) &&
			      nearly_equal(
				      actual.quadrature, expected.quadrature) &&
			      v34_nonlinear_project_power(
				      V34_NONLINEAR_THETA_5_OVER_16,
				      MEAN_ENERGIES[energy_index], input_power,
				      &projected_power) ==
				      V34_NONLINEAR_OK &&
			      nearly_equal(projected_power, expected_power) &&
			      v34_nonlinear_unproject(
				      V34_NONLINEAR_THETA_5_OVER_16,
				      MEAN_ENERGIES[energy_index], &actual,
				      &unprojected) ==
				      V34_NONLINEAR_OK &&
			      nearly_equal(unprojected.in_phase, in_phase) &&
			      nearly_equal(
				      unprojected.quadrature, quadrature),
			      "generated projection %zu/%u changed", energy_index,
			      sample);
			cross_product =
				(long double)actual.in_phase * input.quadrature -
				(long double)actual.quadrature * input.in_phase;
			CHECK(fabsl(cross_product) <=
				      2e-10L *
					      fmaxl(1.0L, fabsl(
							(long double)actual.in_phase *
							input.quadrature)),
			      "projection %zu/%u changed symbol phase",
			      energy_index, sample);
			nonzero_count += input.in_phase != 0 || input.quadrature != 0;
		}
	}
	CHECK(nonzero_count == ARRAY_LENGTH(MEAN_ENERGIES) * 512u,
	      "generated projection sweep missed a radial case");
	return true;
}

static double point_log_power(const struct qam_point *point)
{
	double maximum_axis = fmax(fabs(point->in_phase),
				   fabs(point->quadrature));
	double minimum_axis;
	double ratio;

	if (maximum_axis == 0.0)
		return -INFINITY;
	minimum_axis = fmin(fabs(point->in_phase),
			    fabs(point->quadrature));
	ratio = minimum_axis / maximum_axis;
	return 2.0 * log(maximum_axis) + log1p(ratio * ratio);
}

static double reference_log_projection_factor(double zeta)
{
	if (zeta < 1.0)
		return log1p(zeta / 6.0 + zeta * zeta / 120.0);
	return 2.0 * log(zeta) - log(120.0) +
	       log1p(20.0 / zeta + (120.0 / zeta) / zeta);
}

static bool test_inverse_dynamic_range(void)
{
	static const struct {
		struct qam_point projected;
		double mean_squared_magnitude;
	} cases[] = {
		{ { DBL_MIN, -DBL_MIN }, DBL_MAX },
		{ { 1.0e-150, -2.0e-150 }, 1.0e100 },
		{ { 1.0, -2.0 }, 3.0 },
		{ { 1.0e150, -5.0e149 }, 3.0 },
		{ { DBL_MAX, -DBL_MAX }, DBL_MIN },
	};

	for (size_t index = 0; index < ARRAY_LENGTH(cases); ++index) {
		struct qam_point unprojected;
		double projected_log_power =
			point_log_power(&cases[index].projected);
		double unprojected_log_power;
		double log_zeta;
		double zeta;
		double reconstructed_log_power;
		double projected_scale = fmax(
			fabs(cases[index].projected.in_phase),
			fabs(cases[index].projected.quadrature));
		double unprojected_scale;

		CHECK(v34_nonlinear_unproject(
			      V34_NONLINEAR_THETA_5_OVER_16,
			      cases[index].mean_squared_magnitude,
			      &cases[index].projected, &unprojected) ==
			      V34_NONLINEAR_OK &&
		      isfinite(unprojected.in_phase) &&
		      isfinite(unprojected.quadrature),
		      "dynamic inverse case %zu failed", index);
		unprojected_scale = fmax(fabs(unprojected.in_phase),
					 fabs(unprojected.quadrature));
		CHECK(unprojected_scale > 0.0 &&
		      nearly_equal(
			      unprojected.in_phase / unprojected_scale,
			      cases[index].projected.in_phase /
				      projected_scale) &&
		      nearly_equal(
			      unprojected.quadrature / unprojected_scale,
			      cases[index].projected.quadrature /
				      projected_scale),
		      "dynamic inverse case %zu changed phase", index);
		unprojected_log_power = point_log_power(&unprojected);
		log_zeta = log(5.0 / 16.0) + unprojected_log_power -
			log(cases[index].mean_squared_magnitude);
		zeta = exp(log_zeta);
		reconstructed_log_power = unprojected_log_power;
		if (zeta != 0.0)
			reconstructed_log_power +=
				2.0 * reference_log_projection_factor(zeta);
		CHECK(fabs(reconstructed_log_power - projected_log_power) <=
			      8e-13 *
				      fmax(1.0, fabs(projected_log_power)),
		      "dynamic inverse case %zu changed radius", index);
	}
	return true;
}

static bool test_validation(void)
{
	const struct v34_q7_point input = { INT32_MAX, INT32_MAX };
	struct qam_point output = { 123.0, -456.0 };
	const struct qam_point saved = output;
	struct qam_point projected = { 1.0, -2.0 };
	double projected_power = 789.0;

	CHECK(v34_nonlinear_parameter_is_valid(
		      V34_NONLINEAR_THETA_ZERO) &&
	      v34_nonlinear_parameter_is_valid(
		      V34_NONLINEAR_THETA_5_OVER_16) &&
	      !v34_nonlinear_parameter_is_valid(
		      (enum v34_nonlinear_parameter)2) &&
	      v34_nonlinear_configuration_is_valid(
		      V34_NONLINEAR_THETA_ZERO, 1.0) &&
	      v34_nonlinear_configuration_is_valid(
		      V34_NONLINEAR_THETA_5_OVER_16, DBL_MAX) &&
	      !v34_nonlinear_configuration_is_valid(
		      V34_NONLINEAR_THETA_5_OVER_16, 0.0) &&
	      !v34_nonlinear_configuration_is_valid(
		      V34_NONLINEAR_THETA_5_OVER_16, NAN),
	      "non-linear parameter validation changed");
	CHECK(v34_nonlinear_encode(
		      (enum v34_nonlinear_parameter)2, 1.0,
		      &input, &output) ==
		      V34_NONLINEAR_INVALID_ARGUMENT &&
	      v34_nonlinear_encode(
		      V34_NONLINEAR_THETA_5_OVER_16, 0.0,
		      &input, &output) ==
		      V34_NONLINEAR_INVALID_ARGUMENT &&
	      v34_nonlinear_encode(
		      V34_NONLINEAR_THETA_5_OVER_16, -1.0,
		      &input, &output) ==
		      V34_NONLINEAR_INVALID_ARGUMENT &&
	      v34_nonlinear_encode(
		      V34_NONLINEAR_THETA_5_OVER_16, NAN,
		      &input, &output) ==
		      V34_NONLINEAR_INVALID_ARGUMENT &&
	      v34_nonlinear_encode(
		      V34_NONLINEAR_THETA_5_OVER_16, INFINITY,
		      &input, &output) ==
		      V34_NONLINEAR_INVALID_ARGUMENT &&
	      v34_nonlinear_encode(
		      V34_NONLINEAR_THETA_ZERO, 1.0, NULL,
		      &output) ==
		      V34_NONLINEAR_INVALID_ARGUMENT &&
	      v34_nonlinear_encode(
		      V34_NONLINEAR_THETA_ZERO, 1.0, &input,
		      NULL) ==
		      V34_NONLINEAR_INVALID_ARGUMENT &&
	      output.in_phase == saved.in_phase &&
	      output.quadrature == saved.quadrature,
	      "invalid projection mutated its destination");
	CHECK(v34_nonlinear_project_power(
		      (enum v34_nonlinear_parameter)2, 1.0, 1.0,
		      &projected_power) ==
		      V34_NONLINEAR_INVALID_ARGUMENT &&
	      v34_nonlinear_project_power(
		      V34_NONLINEAR_THETA_5_OVER_16, 0.0, 1.0,
		      &projected_power) ==
		      V34_NONLINEAR_INVALID_ARGUMENT &&
	      v34_nonlinear_project_power(
		      V34_NONLINEAR_THETA_5_OVER_16, 1.0, -1.0,
		      &projected_power) ==
		      V34_NONLINEAR_INVALID_ARGUMENT &&
	      v34_nonlinear_project_power(
		      V34_NONLINEAR_THETA_5_OVER_16, 1.0, NAN,
		      &projected_power) ==
		      V34_NONLINEAR_INVALID_ARGUMENT &&
	      v34_nonlinear_project_power(
		      V34_NONLINEAR_THETA_ZERO, 1.0, 1.0, NULL) ==
		      V34_NONLINEAR_INVALID_ARGUMENT &&
		      projected_power == 789.0,
	      "invalid energy projection mutated its destination");
	CHECK(v34_nonlinear_encode(
		      V34_NONLINEAR_THETA_5_OVER_16, DBL_MIN,
		      &input, &output) ==
		      V34_NONLINEAR_NUMERIC_FAILURE &&
		      output.in_phase == saved.in_phase &&
		      output.quadrature == saved.quadrature,
	      "overflowing projection mutated its destination");
	CHECK(v34_nonlinear_project_power(
		      V34_NONLINEAR_THETA_5_OVER_16, DBL_MIN,
		      DBL_MAX, &projected_power) ==
		      V34_NONLINEAR_NUMERIC_FAILURE &&
		      projected_power == 789.0,
	      "overflowing energy projection mutated its destination");
	CHECK(v34_nonlinear_unproject(
		      (enum v34_nonlinear_parameter)2, 1.0,
		      &projected, &output) ==
		      V34_NONLINEAR_INVALID_ARGUMENT &&
	      v34_nonlinear_unproject(
		      V34_NONLINEAR_THETA_5_OVER_16, 0.0,
		      &projected, &output) ==
		      V34_NONLINEAR_INVALID_ARGUMENT &&
	      v34_nonlinear_unproject(
		      V34_NONLINEAR_THETA_5_OVER_16, 1.0,
		      &(struct qam_point){ NAN, 0.0 }, &output) ==
		      V34_NONLINEAR_INVALID_ARGUMENT &&
	      v34_nonlinear_unproject(
		      V34_NONLINEAR_THETA_5_OVER_16, 1.0,
		      &(struct qam_point){ 0.0, INFINITY },
		      &output) == V34_NONLINEAR_INVALID_ARGUMENT &&
	      v34_nonlinear_unproject(
		      V34_NONLINEAR_THETA_ZERO, 1.0, NULL,
		      &output) == V34_NONLINEAR_INVALID_ARGUMENT &&
	      v34_nonlinear_unproject(
		      V34_NONLINEAR_THETA_ZERO, 1.0, &projected,
		      NULL) == V34_NONLINEAR_INVALID_ARGUMENT &&
	      output.in_phase == saved.in_phase &&
	      output.quadrature == saved.quadrature,
	      "invalid inverse projection mutated its destination");
	CHECK(v34_nonlinear_unproject(
		      V34_NONLINEAR_THETA_ZERO, 1.0,
		      &projected, &projected) == V34_NONLINEAR_OK &&
	      projected.in_phase == 1.0 && projected.quadrature == -2.0,
	      "in-place inverse projection changed its input");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "normative projection", test_normative_projection },
		{ "disabled projection", test_disabled_projection },
		{ "generated projection sweep", test_generated_sweep },
		{ "inverse dynamic range", test_inverse_dynamic_range },
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
