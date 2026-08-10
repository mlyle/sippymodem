// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_v34_symbol_power.h>

#include "test.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

enum {
	GENERATED_SYMBOLS = 4096,
};

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
