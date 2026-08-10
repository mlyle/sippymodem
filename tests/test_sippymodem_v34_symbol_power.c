// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_v34_symbol_power.h>

#include "test.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

enum {
	GENERATED_SYMBOLS = 4096,
	NORMALIZATION_SYMBOLS = 256,
};

static bool nearly_equal(double actual, long double expected)
{
	long double scale = fmaxl(1.0L, fabsl(expected));

	return fabsl((long double)actual - expected) <= 2e-12L * scale;
}

static bool measurements_are_equal(
	const struct sippymodem_v34_symbol_power_measurement *left,
	const struct sippymodem_v34_symbol_power_measurement *right)
{
	return left->mean_squared_magnitude ==
		       right->mean_squared_magnitude &&
	       left->symbol_count == right->symbol_count;
}

static uint32_t next_random(uint32_t *state)
{
	*state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
	return *state;
}

static bool test_generated_mean(void)
{
	struct sippymodem_v34_symbol_power_measurement q7_measurement;
	struct sippymodem_v34_symbol_power_measurement point_measurement;
	uint32_t random = UINT32_C(0x7f08c4d3);
	long double sum = 0.0L;

	CHECK(sippymodem_v34_symbol_power_measurement_reset(
		      &q7_measurement) == SIPPYMODEM_V34_SYMBOL_POWER_OK &&
	      sippymodem_v34_symbol_power_measurement_reset(
		      &point_measurement) == SIPPYMODEM_V34_SYMBOL_POWER_OK,
	      "symbol-power measurements did not reset");
	for (uint32_t index = 0; index < GENERATED_SYMBOLS; ++index) {
		struct sippymodem_v34_q7_point q7 = {
			.in_phase =
				(int32_t)(next_random(&random) % 8193u) - 4096,
			.quadrature =
				(int32_t)(next_random(&random) % 8193u) - 4096,
		};
		struct sippymodem_qam_point point = {
			.in_phase =
				(double)q7.in_phase / SIPPYMODEM_V34_Q7_SCALE,
			.quadrature =
				(double)q7.quadrature / SIPPYMODEM_V34_Q7_SCALE,
		};

		sum += (long double)point.in_phase * point.in_phase +
		       (long double)point.quadrature * point.quadrature;
		CHECK(sippymodem_v34_symbol_power_observe_q7(
			      &q7_measurement, &q7) ==
			      SIPPYMODEM_V34_SYMBOL_POWER_OK &&
		      sippymodem_v34_symbol_power_observe_point(
			      &point_measurement, &point) ==
			      SIPPYMODEM_V34_SYMBOL_POWER_OK,
		      "symbol-power observation failed at %u", index);
	}
	CHECK(measurements_are_equal(&q7_measurement, &point_measurement) &&
	      q7_measurement.symbol_count == GENERATED_SYMBOLS &&
	      fabsl((long double)q7_measurement.mean_squared_magnitude -
		    sum / GENERATED_SYMBOLS) <=
		      2e-12L * fmaxl(1.0L, sum / GENERATED_SYMBOLS),
	      "online symbol-power mean changed");
	return true;
}

static bool test_zero_power(void)
{
	struct sippymodem_v34_symbol_power_measurement measurement = { 0 };
	const struct sippymodem_v34_q7_point q7 = { 0, 0 };
	const struct sippymodem_qam_point point = { 0.0, 0.0 };

	CHECK(sippymodem_v34_symbol_power_measurement_is_valid(&measurement) &&
	      sippymodem_v34_symbol_power_observe_q7(&measurement, &q7) ==
		      SIPPYMODEM_V34_SYMBOL_POWER_OK &&
	      sippymodem_v34_symbol_power_observe_point(
		      &measurement, &point) ==
		      SIPPYMODEM_V34_SYMBOL_POWER_OK &&
	      measurement.symbol_count == 2u &&
	      measurement.mean_squared_magnitude == 0.0 &&
	      sippymodem_v34_symbol_power_measurement_is_valid(&measurement),
	      "zero-power observations changed measurement semantics");
	return true;
}

static bool test_normalized_mean(void)
{
	struct sippymodem_v34_frame_geometry geometry;
	struct sippymodem_v34_mapping_parameters mapping;
	struct sippymodem_v34_symbol_power_measurement precoded = { 0 };
	struct sippymodem_v34_symbol_power_measurement projected = { 0 };
	struct sippymodem_v34_symbol_power_measurement normalized = { 0 };
	struct sippymodem_v34_q7_point input[NORMALIZATION_SYMBOLS];
	struct sippymodem_qam_point encoded[NORMALIZATION_SYMBOLS];
	struct sippymodem_v34_power_normalizer normalizer;
	uint32_t random = UINT32_C(0x9126edb7);

	CHECK(sippymodem_v34_frame_geometry_init(
		      &geometry, SIPPYMODEM_V34_SYMBOL_RATE_3000, 8, false) ==
		      SIPPYMODEM_V34_FRAME_SCHEDULE_OK &&
	      sippymodem_v34_mapping_parameters_init(
		      &mapping, &geometry, false) ==
		      SIPPYMODEM_V34_MAPPING_PARAMETERS_OK,
	      "normalization geometry did not initialize");
	for (size_t index = 0; index < ARRAY_LENGTH(input); ++index) {
		input[index] = (struct sippymodem_v34_q7_point){
			.in_phase =
				(int32_t)(next_random(&random) % 2049u) - 1024,
			.quadrature =
				(int32_t)(next_random(&random) % 2049u) - 1024,
		};
		CHECK(sippymodem_v34_symbol_power_observe_q7(
			      &precoded, &input[index]) ==
			      SIPPYMODEM_V34_SYMBOL_POWER_OK,
		      "precoded calibration failed at %zu", index);
	}
	for (size_t index = 0; index < ARRAY_LENGTH(input); ++index) {
		CHECK(sippymodem_v34_nonlinear_encode(
			      SIPPYMODEM_V34_NONLINEAR_THETA_5_OVER_16,
			      precoded.mean_squared_magnitude, &input[index],
			      &encoded[index]) ==
			      SIPPYMODEM_V34_NONLINEAR_ENCODER_OK &&
		      sippymodem_v34_symbol_power_observe_point(
			      &projected, &encoded[index]) ==
			      SIPPYMODEM_V34_SYMBOL_POWER_OK,
		      "projected calibration failed at %zu", index);
	}
	CHECK(sippymodem_v34_power_normalizer_init(
		      &normalizer, &mapping,
		      SIPPYMODEM_V34_NONLINEAR_THETA_5_OVER_16, &precoded,
		      &projected) == SIPPYMODEM_V34_SYMBOL_POWER_OK,
	      "measured normalizer did not initialize");
	for (size_t index = 0; index < ARRAY_LENGTH(encoded); ++index) {
		struct sippymodem_qam_point output;

		CHECK(sippymodem_v34_normalize_power(
			      &normalizer, &encoded[index], &output) ==
			      SIPPYMODEM_V34_SYMBOL_POWER_OK &&
		      nearly_equal(
			      output.in_phase,
			      (long double)encoded[index].in_phase *
				      normalizer.normalization_gain) &&
		      nearly_equal(
			      output.quadrature,
			      (long double)encoded[index].quadrature *
				      normalizer.normalization_gain) &&
		      sippymodem_v34_symbol_power_observe_point(
			      &normalized, &output) ==
			      SIPPYMODEM_V34_SYMBOL_POWER_OK,
		      "normalization failed at %zu", index);
	}
	CHECK(normalized.symbol_count == NORMALIZATION_SYMBOLS &&
	      nearly_equal(normalized.mean_squared_magnitude, 1.0L),
	      "normalized constellation mean is %.17g instead of one",
	      normalized.mean_squared_magnitude);
	{
		const struct sippymodem_qam_point boundary = {
			.in_phase = sqrt(
				normalizer.maximum_projected_point_power),
		};
		const struct sippymodem_qam_point excessive = {
			.in_phase = boundary.in_phase * 1.000001,
		};
		struct sippymodem_qam_point output = { 123.0, -456.0 };

		CHECK(sippymodem_v34_normalize_power(
			      &normalizer, &boundary, &output) ==
			      SIPPYMODEM_V34_SYMBOL_POWER_OK &&
		      nearly_equal(
			      output.in_phase * output.in_phase +
				      output.quadrature * output.quadrature,
			      normalizer.maximum_normalized_point_power),
		      "normalizer rejected its generated hard boundary");
		output = (struct sippymodem_qam_point){ 123.0, -456.0 };
		CHECK(sippymodem_v34_normalize_power(
			      &normalizer, &excessive, &output) ==
			      SIPPYMODEM_V34_SYMBOL_POWER_OUT_OF_RANGE &&
		      output.in_phase == 123.0 && output.quadrature == -456.0,
		      "normalizer accepted a point beyond its hard boundary");
	}
	return true;
}

static bool bytes_are_filled(
	const void *object, size_t size, unsigned char value)
{
	const unsigned char *bytes = object;

	for (size_t index = 0; index < size; ++index) {
		if (bytes[index] != value)
			return false;
	}
	return true;
}

static bool test_normalizer_validation(void)
{
	struct sippymodem_v34_frame_geometry geometry;
	struct sippymodem_v34_mapping_parameters mapping;
	struct sippymodem_v34_symbol_power_measurement precoded = {
		.mean_squared_magnitude = 10.0,
		.symbol_count = 64,
	};
	struct sippymodem_v34_symbol_power_measurement projected = precoded;
	struct sippymodem_v34_symbol_power_measurement invalid;
	struct sippymodem_v34_power_normalizer normalizer;
	struct sippymodem_v34_power_normalizer corrupt;
	struct sippymodem_qam_point output = { 123.0, -456.0 };
	const struct sippymodem_qam_point point = { 1.0, 0.0 };
	const struct sippymodem_qam_point nonfinite = { NAN, 0.0 };
	const struct sippymodem_qam_point overflowing = { DBL_MAX, DBL_MAX };

	CHECK(sippymodem_v34_frame_geometry_init(
		      &geometry, SIPPYMODEM_V34_SYMBOL_RATE_3000, 8, false) ==
		      SIPPYMODEM_V34_FRAME_SCHEDULE_OK &&
	      sippymodem_v34_mapping_parameters_init(
		      &mapping, &geometry, false) ==
		      SIPPYMODEM_V34_MAPPING_PARAMETERS_OK &&
	      sippymodem_v34_power_normalizer_init(
		      &normalizer, &mapping,
		      SIPPYMODEM_V34_NONLINEAR_THETA_ZERO, &precoded,
		      &projected) == SIPPYMODEM_V34_SYMBOL_POWER_OK,
	      "normalizer validation fixture did not initialize");
	memset(&corrupt, 0xa5, sizeof(corrupt));
	invalid = projected;
	invalid.symbol_count++;
	CHECK(sippymodem_v34_power_normalizer_init(
		      NULL, &mapping, SIPPYMODEM_V34_NONLINEAR_THETA_ZERO,
		      &precoded, &projected) ==
		      SIPPYMODEM_V34_SYMBOL_POWER_INVALID_ARGUMENT &&
	      sippymodem_v34_power_normalizer_init(
		      &corrupt, NULL, SIPPYMODEM_V34_NONLINEAR_THETA_ZERO,
		      &precoded, &projected) ==
		      SIPPYMODEM_V34_SYMBOL_POWER_INVALID_ARGUMENT &&
	      sippymodem_v34_power_normalizer_init(
		      &corrupt, &mapping,
		      (enum sippymodem_v34_nonlinear_parameter)2, &precoded,
		      &projected) ==
		      SIPPYMODEM_V34_SYMBOL_POWER_INVALID_ARGUMENT &&
	      sippymodem_v34_power_normalizer_init(
		      &corrupt, &mapping, SIPPYMODEM_V34_NONLINEAR_THETA_ZERO,
		      NULL, &projected) ==
		      SIPPYMODEM_V34_SYMBOL_POWER_INVALID_ARGUMENT &&
	      sippymodem_v34_power_normalizer_init(
		      &corrupt, &mapping, SIPPYMODEM_V34_NONLINEAR_THETA_ZERO,
		      &precoded, &invalid) ==
		      SIPPYMODEM_V34_SYMBOL_POWER_INVALID_ARGUMENT &&
	      bytes_are_filled(&corrupt, sizeof(corrupt), 0xa5),
	      "invalid normalizer initialization changed its destination");
	invalid = projected;
	invalid.mean_squared_magnitude = 11.0;
	CHECK(sippymodem_v34_power_normalizer_init(
		      &corrupt, &mapping, SIPPYMODEM_V34_NONLINEAR_THETA_ZERO,
		      &precoded, &invalid) ==
		      SIPPYMODEM_V34_SYMBOL_POWER_INVALID_ARGUMENT &&
	      bytes_are_filled(&corrupt, sizeof(corrupt), 0xa5),
	      "zero-theta normalizer accepted unequal measurements");
	invalid = precoded;
	invalid.mean_squared_magnitude = 9.0;
	CHECK(sippymodem_v34_power_normalizer_init(
		      &corrupt, &mapping,
		      SIPPYMODEM_V34_NONLINEAR_THETA_5_OVER_16, &precoded,
		      &invalid) ==
		      SIPPYMODEM_V34_SYMBOL_POWER_INVALID_ARGUMENT &&
	      bytes_are_filled(&corrupt, sizeof(corrupt), 0xa5),
	      "nonlinear normalizer accepted reduced projected power");
	invalid = precoded;
	invalid.mean_squared_magnitude = DBL_MIN;
	projected = invalid;
	CHECK(sippymodem_v34_power_normalizer_init(
		      &corrupt, &mapping,
		      SIPPYMODEM_V34_NONLINEAR_THETA_5_OVER_16, &invalid,
		      &projected) ==
		      SIPPYMODEM_V34_SYMBOL_POWER_NUMERIC_FAILURE &&
	      bytes_are_filled(&corrupt, sizeof(corrupt), 0xa5),
	      "overflowing normalizer initialization changed its destination");
	CHECK(!sippymodem_v34_power_normalizer_is_valid(NULL) &&
	      sippymodem_v34_normalize_power(NULL, &point, &output) ==
		      SIPPYMODEM_V34_SYMBOL_POWER_INVALID_ARGUMENT &&
	      sippymodem_v34_normalize_power(&normalizer, NULL, &output) ==
		      SIPPYMODEM_V34_SYMBOL_POWER_INVALID_ARGUMENT &&
	      sippymodem_v34_normalize_power(&normalizer, &point, NULL) ==
		      SIPPYMODEM_V34_SYMBOL_POWER_INVALID_ARGUMENT &&
	      sippymodem_v34_normalize_power(
		      &normalizer, &nonfinite, &output) ==
		      SIPPYMODEM_V34_SYMBOL_POWER_INVALID_ARGUMENT &&
	      output.in_phase == 123.0 && output.quadrature == -456.0,
	      "invalid normalization changed its destination");
	CHECK(sippymodem_v34_normalize_power(
		      &normalizer, &overflowing, &output) ==
		      SIPPYMODEM_V34_SYMBOL_POWER_NUMERIC_FAILURE &&
	      output.in_phase == 123.0 && output.quadrature == -456.0,
	      "overflowing normalization changed its destination");
	corrupt = normalizer;
	corrupt.normalization_gain *= 2.0;
	CHECK(!sippymodem_v34_power_normalizer_is_valid(&corrupt) &&
	      sippymodem_v34_normalize_power(&corrupt, &point, &output) ==
		      SIPPYMODEM_V34_SYMBOL_POWER_INVALID_ARGUMENT &&
	      output.in_phase == 123.0 && output.quadrature == -456.0,
	      "corrupt normalizer changed its destination");
	corrupt = normalizer;
	corrupt.maximum_precoded_point_power *= 2.0;
	CHECK(!sippymodem_v34_power_normalizer_is_valid(&corrupt) &&
	      sippymodem_v34_normalize_power(&corrupt, &point, &output) ==
		      SIPPYMODEM_V34_SYMBOL_POWER_INVALID_ARGUMENT &&
	      output.in_phase == 123.0 && output.quadrature == -456.0,
	      "corrupt normalizer bound changed its destination");
	return true;
}

static bool test_validation(void)
{
	struct sippymodem_v34_symbol_power_measurement measurement = {
		.mean_squared_magnitude = 12.5,
		.symbol_count = 7,
	};
	struct sippymodem_v34_symbol_power_measurement saved = measurement;
	const struct sippymodem_v34_q7_point q7 = { 1, -1 };
	const struct sippymodem_qam_point nonfinite = { NAN, 0.0 };
	const struct sippymodem_qam_point overflowing = { DBL_MAX, DBL_MAX };

	CHECK(!sippymodem_v34_symbol_power_measurement_is_valid(NULL) &&
	      sippymodem_v34_symbol_power_measurement_reset(NULL) ==
		      SIPPYMODEM_V34_SYMBOL_POWER_INVALID_ARGUMENT &&
	      sippymodem_v34_symbol_power_observe_q7(NULL, &q7) ==
		      SIPPYMODEM_V34_SYMBOL_POWER_INVALID_ARGUMENT &&
	      sippymodem_v34_symbol_power_observe_q7(&measurement, NULL) ==
		      SIPPYMODEM_V34_SYMBOL_POWER_INVALID_ARGUMENT &&
	      sippymodem_v34_symbol_power_observe_point(
		      &measurement, NULL) ==
		      SIPPYMODEM_V34_SYMBOL_POWER_INVALID_ARGUMENT &&
	      sippymodem_v34_symbol_power_observe_point(
		      &measurement, &nonfinite) ==
		      SIPPYMODEM_V34_SYMBOL_POWER_INVALID_ARGUMENT &&
	      measurements_are_equal(&measurement, &saved),
	      "invalid symbol-power input changed its measurement");
	CHECK(sippymodem_v34_symbol_power_observe_point(
		      &measurement, &overflowing) ==
		      SIPPYMODEM_V34_SYMBOL_POWER_NUMERIC_FAILURE &&
	      measurements_are_equal(&measurement, &saved),
	      "overflowing symbol power changed its measurement");
	measurement.symbol_count = UINT32_MAX;
	saved = measurement;
	CHECK(sippymodem_v34_symbol_power_measurement_is_valid(&measurement) &&
	      sippymodem_v34_symbol_power_observe_q7(&measurement, &q7) ==
		      SIPPYMODEM_V34_SYMBOL_POWER_OUT_OF_RANGE &&
	      measurements_are_equal(&measurement, &saved),
	      "full symbol-power measurement accepted another point");
	measurement = (struct sippymodem_v34_symbol_power_measurement){
		.mean_squared_magnitude = 1.0,
	};
	CHECK(!sippymodem_v34_symbol_power_measurement_is_valid(&measurement),
	      "empty symbol-power measurement retained a mean");
	measurement = (struct sippymodem_v34_symbol_power_measurement){
		.mean_squared_magnitude = -1.0,
		.symbol_count = 1,
	};
	CHECK(!sippymodem_v34_symbol_power_measurement_is_valid(&measurement) &&
	      sippymodem_v34_symbol_power_observe_q7(&measurement, &q7) ==
		      SIPPYMODEM_V34_SYMBOL_POWER_INVALID_ARGUMENT &&
	      measurement.mean_squared_magnitude == -1.0 &&
	      measurement.symbol_count == 1u &&
	      strcmp(sippymodem_v34_symbol_power_result_name(
		     SIPPYMODEM_V34_SYMBOL_POWER_OK),
		     "ok") == 0 &&
	      strcmp(sippymodem_v34_symbol_power_result_name(
		     SIPPYMODEM_V34_SYMBOL_POWER_INVALID_ARGUMENT),
		     "invalid argument") == 0 &&
	      strcmp(sippymodem_v34_symbol_power_result_name(
		     SIPPYMODEM_V34_SYMBOL_POWER_OUT_OF_RANGE),
		     "out of range") == 0 &&
	      strcmp(sippymodem_v34_symbol_power_result_name(
		     SIPPYMODEM_V34_SYMBOL_POWER_NUMERIC_FAILURE),
		     "numeric failure") == 0 &&
	      strcmp(sippymodem_v34_symbol_power_result_name(
		     (enum sippymodem_v34_symbol_power_result)4),
		     "unknown") == 0,
	      "symbol-power validation or result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "generated mean", test_generated_mean },
		{ "zero power", test_zero_power },
		{ "normalized mean", test_normalized_mean },
		{ "normalizer validation", test_normalizer_validation },
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
