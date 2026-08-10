// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_v32_constellation.h>

#include "test.h"

#include <math.h>
#include <string.h>

static const double TEST_TOLERANCE = 1e-12;

static bool close_to(double actual, double expected)
{
	return fabs(actual - expected) <= TEST_TOLERANCE;
}

static bool check_grid_point(
	uint8_t symbol, int expected_in_phase, int expected_quadrature)
{
	struct sippymodem_qam_point point;
	double normalization = sqrt(18.0);

	CHECK(sippymodem_v32_constellation_point(symbol, &point) ==
		      SIPPYMODEM_V32_CONSTELLATION_OK &&
		      close_to(point.in_phase * normalization,
			       expected_in_phase) &&
		      close_to(point.quadrature * normalization,
			       expected_quadrature),
	      "V.32 signal point %u was %.12f%+.12fi instead of %d%+di",
	      symbol, point.in_phase * normalization,
	      point.quadrature * normalization, expected_in_phase,
	      expected_quadrature);
	return true;
}

static bool test_generated_geometry(void)
{
	static const struct {
		uint8_t symbol;
		int in_phase;
		int quadrature;
	} representatives[] = {
		{ 0, -1, -1 },
		{ 3, -3, -3 },
		{ 5, 1, -3 },
		{ 10, 1, 3 },
		{ 12, -1, 1 },
		{ 15, -3, 3 },
	};
	struct sippymodem_qam_point points[
		SIPPYMODEM_V32_CONSTELLATION_SYMBOLS];
	double maximum_power = 0.0;

	for (size_t index = 0; index < ARRAY_LENGTH(representatives); ++index) {
		CHECK(check_grid_point(
			representatives[index].symbol,
			representatives[index].in_phase,
			representatives[index].quadrature),
		      "V.32 representative point %zu failed", index);
	}
	for (uint8_t symbol = 0;
	     symbol < SIPPYMODEM_V32_CONSTELLATION_SYMBOLS; ++symbol) {
		double power;

		CHECK(sippymodem_v32_constellation_point(
			      symbol, &points[symbol]) ==
			      SIPPYMODEM_V32_CONSTELLATION_OK,
		      "V.32 signal point %u did not generate", symbol);
		power = points[symbol].in_phase * points[symbol].in_phase +
			points[symbol].quadrature * points[symbol].quadrature;
		CHECK(isfinite(power) && power > 0.0 &&
			      power <= 1.0 + TEST_TOLERANCE,
		      "V.32 signal point %u has invalid power %.12f", symbol,
		      power);
		if (power > maximum_power)
			maximum_power = power;
		for (uint8_t previous = 0; previous < symbol; ++previous) {
			CHECK(!close_to(
				      points[symbol].in_phase,
				      points[previous].in_phase) ||
			      !close_to(
				      points[symbol].quadrature,
				      points[previous].quadrature),
			      "V.32 signal points %u and %u coincide", symbol,
			      previous);
		}
	}
	CHECK(close_to(maximum_power, 1.0),
	      "V.32 constellation maximum power was %.12f", maximum_power);
	return true;
}

static bool test_4800_signal_set(void)
{
	static const int expected[4][2] = {
		{ -3, -1 },
		{ 1, -3 },
		{ 3, 1 },
		{ -1, 3 },
	};
	double normalization = sqrt(18.0);

	for (uint8_t quadrant = 0;
	     quadrant < SIPPYMODEM_V32_4800_SIGNAL_POINTS; ++quadrant) {
		struct sippymodem_qam_point point;
		double power;

		CHECK(sippymodem_v32_4800_constellation_point(
			      quadrant, &point) ==
			      SIPPYMODEM_V32_CONSTELLATION_OK &&
			      close_to(point.in_phase * normalization,
				       expected[quadrant][0]) &&
			      close_to(point.quadrature * normalization,
				       expected[quadrant][1]),
		      "V.32 4800 point %u changed", quadrant);
		power = point.in_phase * point.in_phase +
			point.quadrature * point.quadrature;
		CHECK(close_to(power, 5.0 / 9.0),
		      "V.32 4800 point %u power was %.12f", quadrant, power);
	}
	return true;
}

static bool test_7200_signal_set(void)
{
	static const int expected[16][2] = {
		{ 3, -3 }, { -1, 1 }, { -3, 3 }, { 1, -1 },
		{ 3, 1 },  { -1, -3 }, { -3, -1 }, { 1, 3 },
		{ -1, 3 }, { 3, -1 }, { 1, -3 }, { -3, 1 },
		{ -3, -3 }, { 1, 1 }, { 3, 3 }, { -1, -1 },
	};
	struct sippymodem_qam_point points[
		SIPPYMODEM_V32_7200_SIGNAL_POINTS];
	double normalization = sqrt(18.0);

	for (uint8_t symbol = 0;
	     symbol < SIPPYMODEM_V32_7200_SIGNAL_POINTS; ++symbol) {
		CHECK(sippymodem_v32_7200_constellation_point(
			      symbol, &points[symbol]) ==
			      SIPPYMODEM_V32_CONSTELLATION_OK &&
			      close_to(points[symbol].in_phase * normalization,
				       expected[symbol][0]) &&
			      close_to(points[symbol].quadrature * normalization,
				       expected[symbol][1]),
		      "V.32bis 7200 point %u changed", symbol);
		for (uint8_t previous = 0; previous < symbol; ++previous) {
			CHECK(!close_to(points[symbol].in_phase,
					points[previous].in_phase) ||
				      !close_to(points[symbol].quadrature,
					points[previous].quadrature),
			      "V.32bis 7200 points %u and %u coincide", symbol,
			      previous);
		}
	}
	return true;
}

static bool test_validation(void)
{
	struct sippymodem_qam_point point = { 7.0, -9.0 };
	struct sippymodem_qam_point saved = point;

	CHECK(sippymodem_v32_constellation_point(
		      SIPPYMODEM_V32_CONSTELLATION_SYMBOLS, &point) ==
		      SIPPYMODEM_V32_CONSTELLATION_INVALID_ARGUMENT &&
		      memcmp(&point, &saved, sizeof(point)) == 0 &&
		      sippymodem_v32_constellation_point(0, NULL) ==
			      SIPPYMODEM_V32_CONSTELLATION_INVALID_ARGUMENT &&
		      sippymodem_v32_4800_constellation_point(
			      SIPPYMODEM_V32_4800_SIGNAL_POINTS, &point) ==
			      SIPPYMODEM_V32_CONSTELLATION_INVALID_ARGUMENT &&
		      memcmp(&point, &saved, sizeof(point)) == 0 &&
		      sippymodem_v32_7200_constellation_point(
			      SIPPYMODEM_V32_7200_SIGNAL_POINTS, &point) ==
			      SIPPYMODEM_V32_CONSTELLATION_INVALID_ARGUMENT &&
		      memcmp(&point, &saved, sizeof(point)) == 0 &&
		      strcmp(sippymodem_v32_constellation_result_name(
			     SIPPYMODEM_V32_CONSTELLATION_INVALID_ARGUMENT),
			     "invalid argument") == 0,
	      "V.32 constellation validation changed state or diagnostics");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "generated geometry", test_generated_geometry },
		{ "4800 signal set", test_4800_signal_set },
		{ "7200 signal set", test_7200_signal_set },
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
