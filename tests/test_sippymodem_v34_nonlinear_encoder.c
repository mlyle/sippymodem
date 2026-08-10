// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_v34_nonlinear_encoder.h>

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

static struct sippymodem_qam_point reference_projection(
	const struct sippymodem_v34_q7_point *input,
	double mean_squared_magnitude)
{
	long double in_phase =
		(long double)input->in_phase / SIPPYMODEM_V34_Q7_SCALE;
	long double quadrature =
		(long double)input->quadrature / SIPPYMODEM_V34_Q7_SCALE;
	long double energy =
		in_phase * in_phase + quadrature * quadrature;
	long double zeta = 5.0L * energy /
		(16.0L * (long double)mean_squared_magnitude);
	long double projection =
		1.0L + zeta / 6.0L + zeta * zeta / 120.0L;

	return (struct sippymodem_qam_point){
		.in_phase = (double)(in_phase * projection),
		.quadrature = (double)(quadrature * projection),
	};
}

static bool test_normative_projection(void)
{
	static const struct {
		struct sippymodem_v34_q7_point input;
		double mean_squared_magnitude;
	} cases[] = {
		{ { 0, 0 }, 25.0 },
		{ { 3 * SIPPYMODEM_V34_Q7_SCALE,
		    4 * SIPPYMODEM_V34_Q7_SCALE }, 25.0 },
		{ { -6 * SIPPYMODEM_V34_Q7_SCALE,
		    8 * SIPPYMODEM_V34_Q7_SCALE }, 25.0 },
		{ { 17 * SIPPYMODEM_V34_Q7_SCALE + 3,
		    -9 * SIPPYMODEM_V34_Q7_SCALE - 61 }, 419.25 },
	};

	for (size_t index = 0; index < ARRAY_LENGTH(cases); ++index) {
		struct sippymodem_qam_point expected = reference_projection(
			&cases[index].input,
			cases[index].mean_squared_magnitude);
		struct sippymodem_qam_point actual;

		CHECK(sippymodem_v34_nonlinear_encode(
			      SIPPYMODEM_V34_NONLINEAR_THETA_5_OVER_16,
			      cases[index].mean_squared_magnitude,
			      &cases[index].input, &actual) ==
			      SIPPYMODEM_V34_NONLINEAR_ENCODER_OK &&
		      nearly_equal(actual.in_phase, expected.in_phase) &&
		      nearly_equal(actual.quadrature, expected.quadrature),
		      "non-linear projection case %zu changed", index);
	}
	return true;
}

static bool test_disabled_projection(void)
{
	static const struct sippymodem_v34_q7_point inputs[] = {
		{ 0, 0 },
		{ 1, -1 },
		{ 127, -129 },
		{ INT32_MAX, INT32_MIN },
	};

	for (size_t index = 0; index < ARRAY_LENGTH(inputs); ++index) {
		struct sippymodem_qam_point output;

		CHECK(sippymodem_v34_nonlinear_encode(
			      SIPPYMODEM_V34_NONLINEAR_THETA_ZERO, 1.0,
			      &inputs[index], &output) ==
			      SIPPYMODEM_V34_NONLINEAR_ENCODER_OK &&
		      output.in_phase ==
			      (double)inputs[index].in_phase /
				      SIPPYMODEM_V34_Q7_SCALE &&
		      output.quadrature ==
			      (double)inputs[index].quadrature /
				      SIPPYMODEM_V34_Q7_SCALE,
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
		2.0 * SIPPYMODEM_V34_CHANNEL_COORDINATE_LIMIT *
			SIPPYMODEM_V34_CHANNEL_COORDINATE_LIMIT,
	};
	uint32_t random = UINT32_C(0x46a091bf);
	size_t nonzero_count = 0;

	for (size_t energy_index = 0;
	     energy_index < ARRAY_LENGTH(MEAN_ENERGIES); ++energy_index) {
		for (unsigned int sample = 0; sample < 512; ++sample) {
			struct sippymodem_v34_q7_point input = {
				.in_phase = (int32_t)(next_random(&random) % 65281u) -
					32768,
				.quadrature =
					(int32_t)(next_random(&random) % 65281u) -
					32768,
			};
			struct sippymodem_qam_point expected =
				reference_projection(
					&input, MEAN_ENERGIES[energy_index]);
			struct sippymodem_qam_point actual;
			long double cross_product;

			CHECK(sippymodem_v34_nonlinear_encode(
				      SIPPYMODEM_V34_NONLINEAR_THETA_5_OVER_16,
				      MEAN_ENERGIES[energy_index], &input,
				      &actual) ==
				      SIPPYMODEM_V34_NONLINEAR_ENCODER_OK &&
			      nearly_equal(actual.in_phase, expected.in_phase) &&
			      nearly_equal(
				      actual.quadrature, expected.quadrature),
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

static bool test_validation(void)
{
	const struct sippymodem_v34_q7_point input = { INT32_MAX, INT32_MAX };
	struct sippymodem_qam_point output = { 123.0, -456.0 };
	const struct sippymodem_qam_point saved = output;

	CHECK(sippymodem_v34_nonlinear_parameter_is_valid(
		      SIPPYMODEM_V34_NONLINEAR_THETA_ZERO) &&
	      sippymodem_v34_nonlinear_parameter_is_valid(
		      SIPPYMODEM_V34_NONLINEAR_THETA_5_OVER_16) &&
	      !sippymodem_v34_nonlinear_parameter_is_valid(
		      (enum sippymodem_v34_nonlinear_parameter)2) &&
	      sippymodem_v34_nonlinear_configuration_is_valid(
		      SIPPYMODEM_V34_NONLINEAR_THETA_ZERO, 1.0) &&
	      sippymodem_v34_nonlinear_configuration_is_valid(
		      SIPPYMODEM_V34_NONLINEAR_THETA_5_OVER_16, DBL_MAX) &&
	      !sippymodem_v34_nonlinear_configuration_is_valid(
		      SIPPYMODEM_V34_NONLINEAR_THETA_5_OVER_16, 0.0) &&
	      !sippymodem_v34_nonlinear_configuration_is_valid(
		      SIPPYMODEM_V34_NONLINEAR_THETA_5_OVER_16, NAN),
	      "non-linear parameter validation changed");
	CHECK(sippymodem_v34_nonlinear_encode(
		      (enum sippymodem_v34_nonlinear_parameter)2, 1.0,
		      &input, &output) ==
		      SIPPYMODEM_V34_NONLINEAR_ENCODER_INVALID_ARGUMENT &&
	      sippymodem_v34_nonlinear_encode(
		      SIPPYMODEM_V34_NONLINEAR_THETA_5_OVER_16, 0.0,
		      &input, &output) ==
		      SIPPYMODEM_V34_NONLINEAR_ENCODER_INVALID_ARGUMENT &&
	      sippymodem_v34_nonlinear_encode(
		      SIPPYMODEM_V34_NONLINEAR_THETA_5_OVER_16, -1.0,
		      &input, &output) ==
		      SIPPYMODEM_V34_NONLINEAR_ENCODER_INVALID_ARGUMENT &&
	      sippymodem_v34_nonlinear_encode(
		      SIPPYMODEM_V34_NONLINEAR_THETA_5_OVER_16, NAN,
		      &input, &output) ==
		      SIPPYMODEM_V34_NONLINEAR_ENCODER_INVALID_ARGUMENT &&
	      sippymodem_v34_nonlinear_encode(
		      SIPPYMODEM_V34_NONLINEAR_THETA_5_OVER_16, INFINITY,
		      &input, &output) ==
		      SIPPYMODEM_V34_NONLINEAR_ENCODER_INVALID_ARGUMENT &&
	      sippymodem_v34_nonlinear_encode(
		      SIPPYMODEM_V34_NONLINEAR_THETA_ZERO, 1.0, NULL,
		      &output) ==
		      SIPPYMODEM_V34_NONLINEAR_ENCODER_INVALID_ARGUMENT &&
	      sippymodem_v34_nonlinear_encode(
		      SIPPYMODEM_V34_NONLINEAR_THETA_ZERO, 1.0, &input,
		      NULL) ==
		      SIPPYMODEM_V34_NONLINEAR_ENCODER_INVALID_ARGUMENT &&
	      output.in_phase == saved.in_phase &&
	      output.quadrature == saved.quadrature,
	      "invalid projection mutated its destination");
	CHECK(sippymodem_v34_nonlinear_encode(
		      SIPPYMODEM_V34_NONLINEAR_THETA_5_OVER_16, DBL_MIN,
		      &input, &output) ==
		      SIPPYMODEM_V34_NONLINEAR_ENCODER_NUMERIC_FAILURE &&
	      output.in_phase == saved.in_phase &&
	      output.quadrature == saved.quadrature,
	      "overflowing projection mutated its destination");
	CHECK(strcmp(
		      sippymodem_v34_nonlinear_encoder_result_name(
			      SIPPYMODEM_V34_NONLINEAR_ENCODER_OK),
		      "ok") == 0 &&
	      strcmp(
		      sippymodem_v34_nonlinear_encoder_result_name(
			      SIPPYMODEM_V34_NONLINEAR_ENCODER_INVALID_ARGUMENT),
		      "invalid argument") == 0 &&
	      strcmp(
		      sippymodem_v34_nonlinear_encoder_result_name(
			      SIPPYMODEM_V34_NONLINEAR_ENCODER_NUMERIC_FAILURE),
		      "numeric failure") == 0 &&
	      strcmp(
		      sippymodem_v34_nonlinear_encoder_result_name(
			      (enum sippymodem_v34_nonlinear_encoder_result)3),
		      "unknown") == 0,
	      "non-linear encoder result names changed");
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
		{ "generated sweep", test_generated_sweep },
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
