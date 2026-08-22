// SPDX-License-Identifier: BSD-3-Clause

#include <constellation.h>

#include "test.h"

#include <limits.h>
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
	struct qam_point point;
	double normalization = sqrt(18.0);

	CHECK(v32_constellation_point(symbol, &point) ==
		      V32_CONSTELLATION_OK &&
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
	struct qam_point points[
		V32_CONSTELLATION_SYMBOLS];
	double maximum_power = 0.0;

	for (size_t index = 0; index < ARRAY_LENGTH(representatives); ++index) {
		CHECK(check_grid_point(
			representatives[index].symbol,
			representatives[index].in_phase,
			representatives[index].quadrature),
		      "V.32 representative point %zu failed", index);
	}
	for (uint8_t symbol = 0;
	     symbol < V32_CONSTELLATION_SYMBOLS; ++symbol) {
		double power;

		CHECK(v32_constellation_point(
			      symbol, &points[symbol]) ==
			      V32_CONSTELLATION_OK,
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
	     quadrant < V32_4800_SIGNAL_POINTS; ++quadrant) {
		struct qam_point point;
		double power;

		CHECK(v32_4800_constellation_point(
			      quadrant, &point) ==
			      V32_CONSTELLATION_OK &&
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

static bool doubled_coordinate(double value, int *coordinate)
{
	double scaled = value * (2.0 * sqrt(18.0));
	long rounded = lround(scaled);

	if (!isfinite(scaled) || rounded < INT_MIN || rounded > INT_MAX ||
	    !close_to(scaled, (double)rounded))
		return false;
	*coordinate = (int)rounded;
	return true;
}

static uint64_t hash_coordinate(uint64_t hash, int coordinate)
{
	hash ^= (uint8_t)coordinate;
	return hash * UINT64_C(1099511628211);
}

static bool test_trellis_signal_sets(void)
{
	static const struct {
		uint8_t data_bits;
		uint8_t signal_points;
		int average_power;
		int maximum_power;
		int minimum_distance;
		uint64_t digest;
	} fixtures[] = {
		{ 3, V32_7200_SIGNAL_POINTS, 40, 72, 16,
		  UINT64_C(0x3aa8e124eb808465) },
		{ 4, V32_9600_TRELLIS_SIGNAL_POINTS, 40, 68, 8,
		  UINT64_C(0x737b3bfd8eddcd45) },
		{ 5, V32_12000_SIGNAL_POINTS, 42, 98, 4,
		  UINT64_C(0x8e4c96d89d69c705) },
		{ 6, V32_14400_SIGNAL_POINTS, 41, 85, 2,
		  UINT64_C(0x7734533b6568a1a5) },
	};

	for (size_t fixture = 0; fixture < ARRAY_LENGTH(fixtures); ++fixture) {
		int coordinate[V32_14400_SIGNAL_POINTS][2];
		int in_phase_sum = 0;
		int quadrature_sum = 0;
		int power_sum = 0;
		int maximum_power = 0;
		int minimum_distance = INT_MAX;
		uint64_t digest = UINT64_C(14695981039346656037);

		for (uint8_t symbol = 0;
		     symbol < fixtures[fixture].signal_points; ++symbol) {
			struct qam_point point;
			int power;

			CHECK(v32_trellis_constellation_point(
				      fixtures[fixture].data_bits, symbol, &point) ==
				      V32_CONSTELLATION_OK &&
			      doubled_coordinate(
				      point.in_phase, &coordinate[symbol][0]) &&
			      doubled_coordinate(
				      point.quadrature, &coordinate[symbol][1]),
			      "V.32bis %u-bit signal point %u did not generate",
			      fixtures[fixture].data_bits, symbol);
			if (fixtures[fixture].data_bits == 3) {
				struct qam_point compatibility_point;

				CHECK(v32_7200_constellation_point(
					      symbol, &compatibility_point) ==
					      V32_CONSTELLATION_OK &&
				      close_to(
					      compatibility_point.in_phase,
					      point.in_phase) &&
				      close_to(
					      compatibility_point.quadrature,
					      point.quadrature),
				      "V.32bis 7200 compatibility point %u changed",
				      symbol);
			}
			in_phase_sum += coordinate[symbol][0];
			quadrature_sum += coordinate[symbol][1];
			power = coordinate[symbol][0] * coordinate[symbol][0] +
				coordinate[symbol][1] * coordinate[symbol][1];
			power_sum += power;
			if (power > maximum_power)
				maximum_power = power;
			digest = hash_coordinate(digest, coordinate[symbol][0]);
			digest = hash_coordinate(digest, coordinate[symbol][1]);
			for (uint8_t previous = 0; previous < symbol; ++previous) {
				int in_phase_difference =
					coordinate[symbol][0] -
					coordinate[previous][0];
				int quadrature_difference =
					coordinate[symbol][1] -
					coordinate[previous][1];
				int distance =
					in_phase_difference * in_phase_difference +
					quadrature_difference * quadrature_difference;

				if (distance < minimum_distance)
					minimum_distance = distance;
			}
		}
		CHECK(in_phase_sum == 0 && quadrature_sum == 0 &&
			      power_sum == fixtures[fixture].average_power *
						   fixtures[fixture].signal_points &&
			      maximum_power == fixtures[fixture].maximum_power &&
			      minimum_distance == fixtures[fixture].minimum_distance &&
			      digest == fixtures[fixture].digest,
		      "V.32bis %u-bit signal geometry changed",
		      fixtures[fixture].data_bits);
	}
	return true;
}

static bool test_trellis_representative_points(void)
{
	static const struct {
		uint8_t data_bits;
		uint8_t signal_symbol;
		int in_phase;
		int quadrature;
	} representatives[] = {
		{ 3, 0, 6, -6 },
		{ 3, 15, -2, -2 },
		{ 4, 0, -8, 2 },
		{ 4, 31, -2, 8 },
		{ 5, 0, 7, 1 },
		{ 5, 63, 3, -5 },
		{ 6, 0, -8, -3 },
		{ 6, 127, -5, 0 },
	};

	for (size_t index = 0; index < ARRAY_LENGTH(representatives); ++index) {
		struct qam_point point;
		int in_phase;
		int quadrature;

		CHECK(v32_trellis_constellation_point(
			      representatives[index].data_bits,
			      representatives[index].signal_symbol, &point) ==
			      V32_CONSTELLATION_OK &&
			      doubled_coordinate(point.in_phase, &in_phase) &&
			      doubled_coordinate(point.quadrature, &quadrature) &&
			      in_phase == representatives[index].in_phase &&
			      quadrature == representatives[index].quadrature,
		      "V.32bis representative point %zu changed", index);
	}
	return true;
}

static bool test_validation(void)
{
	struct qam_point point = { 7.0, -9.0 };
	struct qam_point saved = point;

	CHECK(v32_constellation_point(
		      V32_CONSTELLATION_SYMBOLS, &point) ==
		      V32_CONSTELLATION_INVALID_ARGUMENT &&
		      memcmp(&point, &saved, sizeof(point)) == 0 &&
		      v32_constellation_point(0, NULL) ==
			      V32_CONSTELLATION_INVALID_ARGUMENT &&
		      v32_4800_constellation_point(
			      V32_4800_SIGNAL_POINTS, &point) ==
			      V32_CONSTELLATION_INVALID_ARGUMENT &&
		      memcmp(&point, &saved, sizeof(point)) == 0 &&
		      v32_7200_constellation_point(
			      V32_7200_SIGNAL_POINTS, &point) ==
			      V32_CONSTELLATION_INVALID_ARGUMENT &&
		      v32_trellis_constellation_point(2, 0, &point) ==
			      V32_CONSTELLATION_INVALID_ARGUMENT &&
		      v32_trellis_constellation_point(7, 0, &point) ==
			      V32_CONSTELLATION_INVALID_ARGUMENT &&
		      v32_trellis_constellation_point(
			      3, V32_7200_SIGNAL_POINTS, &point) ==
			      V32_CONSTELLATION_INVALID_ARGUMENT &&
		      v32_trellis_constellation_point(3, 0, NULL) ==
			      V32_CONSTELLATION_INVALID_ARGUMENT &&
		      memcmp(&point, &saved, sizeof(point)) == 0,
	      "V.32 constellation validation changed state");
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
		{ "trellis signal sets", test_trellis_signal_sets },
		{ "trellis representative points",
		  test_trellis_representative_points },
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
