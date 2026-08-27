// SPDX-License-Identifier: BSD-3-Clause

#include <v34_symbol_power.h>

#include "test.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

enum {
	GENERATED_SYMBOLS = 4096,
	NORMALIZATION_SYMBOLS = 256,
};

struct symbol_rate_range {
	enum v34_symbol_rate symbol_rate;
	uint8_t minimum_rate;
	uint8_t maximum_rate;
};

static const struct symbol_rate_range SYMBOL_RATE_RANGES[] = {
	{ V34_SYMBOL_RATE_2400, 1, 9 },
	{ V34_SYMBOL_RATE_2743, 2, 11 },
	{ V34_SYMBOL_RATE_2800, 2, 11 },
	{ V34_SYMBOL_RATE_3000, 2, 12 },
	{ V34_SYMBOL_RATE_3200, 2, 13 },
	{ V34_SYMBOL_RATE_3429, 2, 14 },
};

static bool nearly_equal(double actual, long double expected)
{
	long double scale = fmaxl(1.0L, fabsl(expected));

	return fabsl((long double)actual - expected) <= 2e-12L * scale;
}

static bool measurements_are_equal(
	const struct v34_symbol_power_measurement *left,
	const struct v34_symbol_power_measurement *right)
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
	struct v34_symbol_power_measurement q7_measurement;
	struct v34_symbol_power_measurement point_measurement;
	uint32_t random = UINT32_C(0x7f08c4d3);
	long double sum = 0.0L;

	CHECK(v34_symbol_power_measurement_reset(
		      &q7_measurement) == V34_SYMBOL_POWER_OK &&
	      v34_symbol_power_measurement_reset(
		      &point_measurement) == V34_SYMBOL_POWER_OK,
	      "symbol-power measurements did not reset");
	for (uint32_t index = 0; index < GENERATED_SYMBOLS; ++index) {
		struct v34_q7_point q7 = {
			.in_phase =
				(int32_t)(next_random(&random) % 8193u) - 4096,
			.quadrature =
				(int32_t)(next_random(&random) % 8193u) - 4096,
		};
		struct qam_point point = {
			.in_phase =
				(double)q7.in_phase / V34_Q7_SCALE,
			.quadrature =
				(double)q7.quadrature / V34_Q7_SCALE,
		};

		sum += (long double)point.in_phase * point.in_phase +
		       (long double)point.quadrature * point.quadrature;
		CHECK(v34_symbol_power_observe_q7(
			      &q7_measurement, &q7) ==
			      V34_SYMBOL_POWER_OK &&
		      v34_symbol_power_observe_point(
			      &point_measurement, &point) ==
			      V34_SYMBOL_POWER_OK,
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
	struct v34_symbol_power_measurement measurement = { 0 };
	const struct v34_q7_point q7 = { 0, 0 };
	const struct qam_point point = { 0.0, 0.0 };

	CHECK(v34_symbol_power_measurement_is_valid(&measurement) &&
	      v34_symbol_power_observe_q7(&measurement, &q7) ==
		      V34_SYMBOL_POWER_OK &&
	      v34_symbol_power_observe_point(
		      &measurement, &point) ==
		      V34_SYMBOL_POWER_OK &&
	      measurement.symbol_count == 2u &&
	      measurement.mean_squared_magnitude == 0.0 &&
	      v34_symbol_power_measurement_is_valid(&measurement),
	      "zero-power observations changed measurement semantics");
	return true;
}

static bool test_normalized_mean(void)
{
	struct v34_frame_geometry geometry;
	struct v34_mapping_parameters mapping;
	struct v34_symbol_power_measurement precoded = { 0 };
	struct v34_symbol_power_measurement projected = { 0 };
	struct v34_symbol_power_measurement normalized = { 0 };
	struct v34_q7_point input[NORMALIZATION_SYMBOLS];
	struct qam_point encoded[NORMALIZATION_SYMBOLS];
	struct v34_power_normalizer normalizer;
	uint32_t random = UINT32_C(0x9126edb7);

	CHECK(v34_frame_geometry_init(
		      &geometry, V34_SYMBOL_RATE_3000, 8, false) ==
		      V34_FRAME_SCHEDULE_OK &&
	      v34_mapping_parameters_init(
		      &mapping, &geometry, false) ==
		      V34_MAPPING_PARAMETERS_OK,
	      "normalization geometry did not initialize");
	for (size_t index = 0; index < ARRAY_LENGTH(input); ++index) {
		input[index] = (struct v34_q7_point){
			.in_phase =
				(int32_t)(next_random(&random) % 2049u) - 1024,
			.quadrature =
				(int32_t)(next_random(&random) % 2049u) - 1024,
		};
		CHECK(v34_symbol_power_observe_q7(
			      &precoded, &input[index]) ==
			      V34_SYMBOL_POWER_OK,
		      "precoded calibration failed at %zu", index);
	}
	for (size_t index = 0; index < ARRAY_LENGTH(input); ++index) {
		CHECK(v34_nonlinear_encode(
			      V34_NONLINEAR_THETA_5_OVER_16,
			      precoded.mean_squared_magnitude, &input[index],
			      &encoded[index]) ==
			      V34_NONLINEAR_OK &&
		      v34_symbol_power_observe_point(
			      &projected, &encoded[index]) ==
			      V34_SYMBOL_POWER_OK,
		      "projected calibration failed at %zu", index);
	}
	CHECK(v34_power_normalizer_init(
		      &normalizer, &mapping,
		      V34_NONLINEAR_THETA_5_OVER_16, &precoded,
		      &projected) == V34_SYMBOL_POWER_OK,
	      "measured normalizer did not initialize");
	for (size_t index = 0; index < ARRAY_LENGTH(encoded); ++index) {
		struct qam_point output;

		CHECK(v34_normalize_power(
			      &normalizer, &encoded[index], &output) ==
			      V34_SYMBOL_POWER_OK &&
		      nearly_equal(
			      output.in_phase,
			      (long double)encoded[index].in_phase *
				      normalizer.normalization_gain) &&
		      nearly_equal(
			      output.quadrature,
			      (long double)encoded[index].quadrature *
				      normalizer.normalization_gain) &&
		      v34_symbol_power_observe_point(
			      &normalized, &output) ==
			      V34_SYMBOL_POWER_OK,
		      "normalization failed at %zu", index);
	}
	CHECK(normalized.symbol_count == NORMALIZATION_SYMBOLS &&
	      nearly_equal(normalized.mean_squared_magnitude, 1.0L),
	      "normalized constellation mean is %.17g instead of one",
	      normalized.mean_squared_magnitude);
	{
		const struct qam_point boundary = {
			.in_phase = sqrt(
				normalizer.maximum_projected_point_power),
		};
		const struct qam_point excessive = {
			.in_phase = boundary.in_phase * 1.000001,
		};
		struct qam_point output = { 123.0, -456.0 };

		CHECK(v34_normalize_power(
			      &normalizer, &boundary, &output) ==
			      V34_SYMBOL_POWER_OK &&
		      nearly_equal(
			      output.in_phase * output.in_phase +
				      output.quadrature * output.quadrature,
			      normalizer.maximum_normalized_point_power),
		      "normalizer rejected its generated hard boundary");
		output = (struct qam_point){ 123.0, -456.0 };
		CHECK(v34_normalize_power(
			      &normalizer, &excessive, &output) ==
			      V34_SYMBOL_POWER_OUT_OF_RANGE &&
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

static long double independently_project_power(
	enum v34_nonlinear_parameter parameter,
	long double mean_power, long double point_power)
{
	long double factor = 1.0L;

	if (parameter == V34_NONLINEAR_THETA_5_OVER_16) {
		long double zeta = (5.0L / 16.0L) * point_power / mean_power;

		factor += zeta / 6.0L + zeta * zeta / 120.0L;
	}
	return point_power * factor * factor;
}

static long double independently_bound_precoded_power(
	const struct v34_grid_point *quarter_constellation,
	size_t point_count, int half_grid_step)
{
	long double maximum = 0.0L;

	for (size_t label = 0; label < point_count; ++label) {
		long double in_phase =
			fabsl((long double)quarter_constellation[label].in_phase) +
			half_grid_step;
		long double quadrature =
			fabsl((long double)quarter_constellation[label].quadrature) +
			half_grid_step;
		long double power = in_phase * in_phase +
			quadrature * quadrature;

		if (power > maximum)
			maximum = power;
	}
	return maximum;
}

static bool test_all_power_envelopes(void)
{
	struct v34_grid_point quarter_constellation[
		V34_QUARTER_SUPERCONSTELLATION_POINT_COUNT];
	bool saw_grid_step_2 = false;
	bool saw_grid_step_4 = false;
	size_t configuration_count = 0;

	CHECK(v34_quarter_superconstellation_generate(
		      quarter_constellation,
		      ARRAY_LENGTH(quarter_constellation)) ==
		      V34_CONSTELLATION_OK,
	      "power-envelope constellation did not generate");
	for (size_t range_index = 0;
	     range_index < ARRAY_LENGTH(SYMBOL_RATE_RANGES); ++range_index) {
		const struct symbol_rate_range *range =
			&SYMBOL_RATE_RANGES[range_index];

		for (uint8_t rate = range->minimum_rate;
		     rate <= range->maximum_rate; ++rate) {
			for (unsigned int auxiliary = 0; auxiliary < 2;
			     ++auxiliary) {
				struct v34_frame_geometry geometry;

				CHECK(v34_frame_geometry_init(
					      &geometry, range->symbol_rate, rate,
					      auxiliary != 0) ==
					      V34_FRAME_SCHEDULE_OK,
				      "power geometry %zu/%u/%u failed",
				      range_index, rate, auxiliary);
				for (unsigned int expanded = 0; expanded < 2;
				     ++expanded) {
					struct v34_mapping_parameters
						parameters;

					CHECK(v34_mapping_parameters_init(
						      &parameters, &geometry,
						      expanded != 0) ==
						      V34_MAPPING_PARAMETERS_OK,
					      "power mapping %zu/%u/%u/%u failed",
					      range_index, rate, auxiliary, expanded);
					for (unsigned int nonlinear = 0;
					     nonlinear < 2; ++nonlinear) {
						enum v34_nonlinear_parameter
							parameter = nonlinear != 0 ?
							V34_NONLINEAR_THETA_5_OVER_16 :
							V34_NONLINEAR_THETA_ZERO;
						struct v34_symbol_power_measurement
							precoded = { 1.0, 1 };
						struct v34_symbol_power_measurement
							projected = {
								(double)independently_project_power(
									parameter, 1.0L, 1.0L),
								1,
							};
						struct v34_power_normalizer
							normalizer;
						int half_grid_step =
							parameters.mapping_frame_bits < 56u ?
								1 : 2;
						long double maximum_precoded =
							independently_bound_precoded_power(
								quarter_constellation,
								parameters.constellation_points / 4u,
								half_grid_step);
						long double maximum_projected =
							independently_project_power(
								parameter, 1.0L,
								maximum_precoded);
						long double expected_gain =
							1.0L / sqrtl(
								projected.mean_squared_magnitude);

						CHECK(v34_power_normalizer_init(
							      &normalizer, &parameters,
							      parameter, &precoded,
							      &projected) ==
							      V34_SYMBOL_POWER_OK &&
						      v34_power_normalizer_is_valid(
							      &normalizer) &&
						      nearly_equal(
							      normalizer
								      .maximum_precoded_point_power,
							      maximum_precoded) &&
						      nearly_equal(
							      normalizer
								      .maximum_projected_point_power,
							      maximum_projected) &&
						      nearly_equal(
							      normalizer.normalization_gain,
							      expected_gain) &&
						      nearly_equal(
							      normalizer
								      .maximum_normalized_point_power,
							      maximum_projected /
								      projected
									      .mean_squared_magnitude),
						      "power envelope %zu/%u/%u/%u/%u changed",
						      range_index, rate, auxiliary, expanded,
						      nonlinear);
						saw_grid_step_2 |= half_grid_step == 1;
						saw_grid_step_4 |= half_grid_step == 2;
						configuration_count++;
					}
				}
			}
		}
	}
	CHECK(configuration_count == 520u && saw_grid_step_2 && saw_grid_step_4,
	      "power sweep covered %zu configurations and grid steps %u/%u",
	      configuration_count, saw_grid_step_2, saw_grid_step_4);
	return true;
}

static bool test_normalizer_validation(void)
{
	struct v34_frame_geometry geometry;
	struct v34_mapping_parameters mapping;
	struct v34_symbol_power_measurement precoded = {
		.mean_squared_magnitude = 10.0,
		.symbol_count = 64,
	};
	struct v34_symbol_power_measurement projected = precoded;
	struct v34_symbol_power_measurement invalid;
	struct v34_power_normalizer normalizer;
	struct v34_power_normalizer corrupt;
	struct qam_point output = { 123.0, -456.0 };
	const struct qam_point point = { 1.0, 0.0 };
	const struct qam_point nonfinite = { NAN, 0.0 };
	const struct qam_point overflowing = { DBL_MAX, DBL_MAX };

	CHECK(v34_frame_geometry_init(
		      &geometry, V34_SYMBOL_RATE_3000, 8, false) ==
		      V34_FRAME_SCHEDULE_OK &&
	      v34_mapping_parameters_init(
		      &mapping, &geometry, false) ==
		      V34_MAPPING_PARAMETERS_OK &&
	      v34_power_normalizer_init(
		      &normalizer, &mapping,
		      V34_NONLINEAR_THETA_ZERO, &precoded,
		      &projected) == V34_SYMBOL_POWER_OK,
	      "normalizer validation fixture did not initialize");
	memset(&corrupt, 0xa5, sizeof(corrupt));
	invalid = projected;
	invalid.symbol_count++;
	CHECK(v34_power_normalizer_init(
		      NULL, &mapping, V34_NONLINEAR_THETA_ZERO,
		      &precoded, &projected) ==
		      V34_SYMBOL_POWER_INVALID_ARGUMENT &&
	      v34_power_normalizer_init(
		      &corrupt, NULL, V34_NONLINEAR_THETA_ZERO,
		      &precoded, &projected) ==
		      V34_SYMBOL_POWER_INVALID_ARGUMENT &&
	      v34_power_normalizer_init(
		      &corrupt, &mapping,
		      (enum v34_nonlinear_parameter)2, &precoded,
		      &projected) ==
		      V34_SYMBOL_POWER_INVALID_ARGUMENT &&
	      v34_power_normalizer_init(
		      &corrupt, &mapping, V34_NONLINEAR_THETA_ZERO,
		      NULL, &projected) ==
		      V34_SYMBOL_POWER_INVALID_ARGUMENT &&
	      v34_power_normalizer_init(
		      &corrupt, &mapping, V34_NONLINEAR_THETA_ZERO,
		      &precoded, &invalid) ==
		      V34_SYMBOL_POWER_INVALID_ARGUMENT &&
	      bytes_are_filled(&corrupt, sizeof(corrupt), 0xa5),
	      "invalid normalizer initialization changed its destination");
	invalid = projected;
	invalid.mean_squared_magnitude = 11.0;
	CHECK(v34_power_normalizer_init(
		      &corrupt, &mapping, V34_NONLINEAR_THETA_ZERO,
		      &precoded, &invalid) ==
		      V34_SYMBOL_POWER_INVALID_ARGUMENT &&
	      bytes_are_filled(&corrupt, sizeof(corrupt), 0xa5),
	      "zero-theta normalizer accepted unequal measurements");
	invalid = precoded;
	invalid.mean_squared_magnitude = 9.0;
	CHECK(v34_power_normalizer_init(
		      &corrupt, &mapping,
		      V34_NONLINEAR_THETA_5_OVER_16, &precoded,
		      &invalid) ==
		      V34_SYMBOL_POWER_INVALID_ARGUMENT &&
	      bytes_are_filled(&corrupt, sizeof(corrupt), 0xa5),
	      "nonlinear normalizer accepted reduced projected power");
	invalid = precoded;
	invalid.mean_squared_magnitude = DBL_MIN;
	projected = invalid;
	CHECK(v34_power_normalizer_init(
		      &corrupt, &mapping,
		      V34_NONLINEAR_THETA_5_OVER_16, &invalid,
		      &projected) ==
		      V34_SYMBOL_POWER_NUMERIC_FAILURE &&
	      bytes_are_filled(&corrupt, sizeof(corrupt), 0xa5),
	      "overflowing normalizer initialization changed its destination");
	CHECK(!v34_power_normalizer_is_valid(NULL) &&
	      v34_normalize_power(NULL, &point, &output) ==
		      V34_SYMBOL_POWER_INVALID_ARGUMENT &&
	      v34_normalize_power(&normalizer, NULL, &output) ==
		      V34_SYMBOL_POWER_INVALID_ARGUMENT &&
	      v34_normalize_power(&normalizer, &point, NULL) ==
		      V34_SYMBOL_POWER_INVALID_ARGUMENT &&
	      v34_normalize_power(
		      &normalizer, &nonfinite, &output) ==
		      V34_SYMBOL_POWER_INVALID_ARGUMENT &&
	      output.in_phase == 123.0 && output.quadrature == -456.0,
	      "invalid normalization changed its destination");
	CHECK(v34_normalize_power(
		      &normalizer, &overflowing, &output) ==
		      V34_SYMBOL_POWER_NUMERIC_FAILURE &&
	      output.in_phase == 123.0 && output.quadrature == -456.0,
	      "overflowing normalization changed its destination");
	corrupt = normalizer;
	corrupt.normalization_gain *= 2.0;
	CHECK(!v34_power_normalizer_is_valid(&corrupt) &&
	      v34_normalize_power(&corrupt, &point, &output) ==
		      V34_SYMBOL_POWER_INVALID_ARGUMENT &&
	      output.in_phase == 123.0 && output.quadrature == -456.0,
	      "corrupt normalizer changed its destination");
	corrupt = normalizer;
	corrupt.maximum_precoded_point_power *= 2.0;
	CHECK(!v34_power_normalizer_is_valid(&corrupt) &&
	      v34_normalize_power(&corrupt, &point, &output) ==
		      V34_SYMBOL_POWER_INVALID_ARGUMENT &&
	      output.in_phase == 123.0 && output.quadrature == -456.0,
	      "corrupt normalizer bound changed its destination");
	return true;
}

static bool test_validation(void)
{
	struct v34_symbol_power_measurement measurement = {
		.mean_squared_magnitude = 12.5,
		.symbol_count = 7,
	};
	struct v34_symbol_power_measurement saved = measurement;
	const struct v34_q7_point q7 = { 1, -1 };
	const struct qam_point nonfinite = { NAN, 0.0 };
	const struct qam_point overflowing = { DBL_MAX, DBL_MAX };

	CHECK(!v34_symbol_power_measurement_is_valid(NULL) &&
	      v34_symbol_power_measurement_reset(NULL) ==
		      V34_SYMBOL_POWER_INVALID_ARGUMENT &&
	      v34_symbol_power_observe_q7(NULL, &q7) ==
		      V34_SYMBOL_POWER_INVALID_ARGUMENT &&
	      v34_symbol_power_observe_q7(&measurement, NULL) ==
		      V34_SYMBOL_POWER_INVALID_ARGUMENT &&
	      v34_symbol_power_observe_point(
		      &measurement, NULL) ==
		      V34_SYMBOL_POWER_INVALID_ARGUMENT &&
	      v34_symbol_power_observe_point(
		      &measurement, &nonfinite) ==
		      V34_SYMBOL_POWER_INVALID_ARGUMENT &&
	      measurements_are_equal(&measurement, &saved),
	      "invalid symbol-power input changed its measurement");
	CHECK(v34_symbol_power_observe_point(
		      &measurement, &overflowing) ==
		      V34_SYMBOL_POWER_NUMERIC_FAILURE &&
	      measurements_are_equal(&measurement, &saved),
	      "overflowing symbol power changed its measurement");
	measurement.symbol_count = UINT32_MAX;
	saved = measurement;
	CHECK(v34_symbol_power_measurement_is_valid(&measurement) &&
	      v34_symbol_power_observe_q7(&measurement, &q7) ==
		      V34_SYMBOL_POWER_OUT_OF_RANGE &&
	      measurements_are_equal(&measurement, &saved),
	      "full symbol-power measurement accepted another point");
	measurement = (struct v34_symbol_power_measurement){
		.mean_squared_magnitude = 1.0,
	};
	CHECK(!v34_symbol_power_measurement_is_valid(&measurement),
	      "empty symbol-power measurement retained a mean");
	measurement = (struct v34_symbol_power_measurement){
		.mean_squared_magnitude = -1.0,
		.symbol_count = 1,
	};
	CHECK(!v34_symbol_power_measurement_is_valid(&measurement) &&
	      v34_symbol_power_observe_q7(&measurement, &q7) ==
		      V34_SYMBOL_POWER_INVALID_ARGUMENT &&
	      measurement.mean_squared_magnitude == -1.0 &&
	      measurement.symbol_count == 1u,
	      "symbol-power validation changed");
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
		{ "all power envelopes", test_all_power_envelopes },
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
